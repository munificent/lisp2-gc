#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STACK_MAX 256
#define HEAP_SIZE (1024 * 1024)

// Two kinds of objects are supported: a (boxed) integer, and a pair of
// references to other objects.
typedef enum {
  OBJ_INT,
  OBJ_PAIR
} ObjectType;

// A single object in the VM.
typedef struct sObject {
  // The type of this object.
  ObjectType type;

  // During the sweep phase of garbage collection, this will be non-NULL if the
  // object was reached, otherwise it will be NULL. Before compaction, this
  // will store the address that the object will end up at after compaction.
  // Once garbage collection is done, this is reset to NULL. It is only used
  // during collection.
  void* moveTo;

  // The type-specific data for the object.
  union {
    // OBJ_INT.
    int value;

    // OBJ_PAIR.
    struct {
      struct sObject* head;
      struct sObject* tail;
    };
  };
} Object;

// A virtual machine with its own virtual stack and heap. All objects live on
// the heap. The stack just points to them.
typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  // The beginning of the contiguous heap of memory that objects are allocated
  // from.
  void* heap;

  // The beginning of the next chunk of memory to be allocated from the heap.
  void* next;
} VM;

void assertLive(VM* vm, long expectedCount) {
  long actualCount = (vm->next - vm->heap) / sizeof(Object);
  if (actualCount == expectedCount) {
    printf("PASS: Expected and found %ld live objects.\n", expectedCount);
  } else {
    printf("Expected heap to contain %ld objects, but had %ld.\n",
           expectedCount, actualCount);
    exit(1);
  }
}

// Creates a new VM with an empty stack and an empty (but allocated) heap.
VM* newVM() {
  VM* vm = malloc(sizeof(VM));
  vm->stackSize = 0;

  vm->heap = malloc(HEAP_SIZE);
  vm->next = vm->heap;

  return vm;
}

// Pushes a reference to [value] onto the VM's stack.
void push(VM* vm, Object* value) {
  if (vm->stackSize == STACK_MAX) {
    perror("Stack overflow.\n");
    exit(1);
  }

  vm->stack[vm->stackSize++] = value;
}

// Pops the top-most reference to an object from the stack.
Object* pop(VM* vm) {
  return vm->stack[--vm->stackSize];
}

// Marks [object] as being reachable and still (potentially) in use.
void mark(Object* object) {
  // If already marked, we're done. Check this first to avoid recursing
  // on cycles in the object graph.
  if (object->moveTo) return;

  // Any non-zero pointer indicates the object was reached. For no particular
  // reason, we use the object's own address as the marked value.
  object->moveTo = object;

  // Recurse into the object's fields.
  if (object->type == OBJ_PAIR) {
    mark(object->head);
    mark(object->tail);
  }
}

// The mark phase of garbage collection. Starting at the roots (in this case,
// just the stack), recursively walks all reachable objects in the VM.
void markAll(VM* vm) {
  for (int i = 0; i < vm->stackSize; i++) {
    mark(vm->stack[i]);
  }
}

// Phase one of the LISP2 algorithm. Walks the entire heap and, for each live
// object, calculates where it will end up after compaction has moved it.
//
// Returns the address of the end of the live section of the heap after
// compaction is done.
void* calculateNewLocations(VM* vm) {
  // Calculate the new locations of the objects in the heap.
  void* from = vm->heap;
  void* to = vm->heap;
  while (from < vm->next) {
    Object* object = (Object*)from;
    if (object->moveTo) {
      object->moveTo = to;

      // We increase the destination address only when we pass a live object.
      // This effectively slides objects up on memory over dead ones.
      to += sizeof(Object);
    }

    from += sizeof(Object);
  }

  return to;
}

// Phase two of the LISP2 algorithm. Now that we know where each object *will*
// be, find every reference to an object and update that pointer to the new
// value. This includes reference in the stack, as well as fields in (live)
// objects that point to other objects.
//
// We do this *before* compaction. Since an object's new location is stored in
// [object.moveTo] in the object itself, this needs to be able to find the
// object. Doing this process before objects have been moved ensures we can
// still find them by traversing the *old* pointers.
void updateAllObjectPointers(VM* vm) {
  // Walk the stack.
  for (int i = 0; i < vm->stackSize; i++) {
    // Update the pointer on the stack to point to the object's new compacted
    // location.
    vm->stack[i] = vm->stack[i]->moveTo;
  }

  // Walk the heap, fixing fields in live pairs.
  void* from = vm->heap;
  while (from < vm->next) {
    Object* object = (Object*)from;

    if (object->moveTo && object->type == OBJ_PAIR) {
      object->head = object->head->moveTo;
      object->tail = object->tail->moveTo;
    }

    from += sizeof(Object);
  }
}

// Phase three of the LISP2 algorithm. Now that we know where everything will
// end up, and all of the pointers have been fixed, actually slide all of the
// live objects up in memory.
void compact(VM* vm) {
  void* from = vm->heap;

  while (from < vm->next) {
    Object* object = (Object*)from;
    if (object->moveTo) {
      // Move the object from its old location to its new location.
      Object* to = object->moveTo;
      memmove(to, object, sizeof(Object));

      // Clear the mark.
      to->moveTo = NULL;
    }

    from += sizeof(Object);
  }
}

// Free memory for all unused objects.
void gc(VM* vm) {
  // Find out which objects are still in use.
  markAll(vm);

  // Determine where they will end up.
  void* end = calculateNewLocations(vm);

  // Fix the references to them.
  updateAllObjectPointers(vm);

  // Compact the memory.
  compact(vm);

  // Update the end of the heap to the new post-compaction end.
  vm->next = end;

  printf("%ld live bytes after collection.\n", vm->next - vm->heap);
}

// Create a new object.
//
// This does *not* root the object, so it's important that a GC does not happen
// between calling this and adding a reference to the object in a field or on
// the stack.
Object* newObject(VM* vm, ObjectType type) {
  if (vm->next + sizeof(Object) > vm->heap + HEAP_SIZE) {
    gc(vm);

    // If there still isn't room after collection, we can't fit it.
    if (vm->next + sizeof(Object) > vm->heap + HEAP_SIZE) {
      perror("Out of memory");
      exit(1);
    }
  }

  Object* object = (Object*)vm->next;
  vm->next += sizeof(Object);

  object->type = type;
  object->moveTo = NULL;

  return object;
}

// Creates a new int object and pushes it onto the stack.
void pushInt(VM* vm, int intValue) {
  Object* object = newObject(vm, OBJ_INT);
  object->value = intValue;

  push(vm, object);
}

// Creates a new pair object. The field values for the pair are popped from the
// stack, then the resulting pair is pushed.
Object* pushPair(VM* vm) {
  // Create the pair before popping the fields. This ensures the fields don't
  // get collected if creating the pair triggers a GC.
  Object* object = newObject(vm, OBJ_PAIR);

  object->tail = pop(vm);
  object->head = pop(vm);

  push(vm, object);
  return object;
}

// Prints [object].
void objectPrint(Object* object) {
  switch (object->type) {
    case OBJ_INT:
      printf("%d", object->value);
      break;

    case OBJ_PAIR:
      printf("(");
      objectPrint(object->head);
      printf(", ");
      objectPrint(object->tail);
      printf(")");
      break;
  }
}

// Deallocates all memory used by [vm].
void freeVM(VM *vm) {
  free(vm->heap);
  free(vm);
}

void test1() {
  printf("Test 1: Objects on stack are preserved.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);

  gc(vm);
  assertLive(vm, 2);
  freeVM(vm);
}

void test2() {
  printf("Test 2: Unreached objects are collected.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pop(vm);
  pop(vm);

  gc(vm);
  assertLive(vm, 0);
  freeVM(vm);
}

void test3() {
  printf("Test 3: Reach nested objects.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  pushPair(vm);
  pushPair(vm);

  gc(vm);
  assertLive(vm, 7);
  freeVM(vm);
}

void test4() {
  printf("Test 4: Handle cycles.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  Object* a = pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  Object* b = pushPair(vm);

  a->tail = b;
  b->tail = a;

  gc(vm);
  assertLive(vm, 4);
  freeVM(vm);
}

void perfTest() {
  printf("Performance Test.\n");
  VM* vm = newVM();

  for (int i = 0; i < 100000; i++) {
    for (int j = 0; j < 20; j++) {
      pushInt(vm, i);
    }

    for (int k = 0; k < 20; k++) {
      pop(vm);
    }
  }
  freeVM(vm);
}

int main(int argc, const char * argv[]) {
  test1();
  test2();
  test3();
  test4();
  perfTest();
  
  return 0;
}
