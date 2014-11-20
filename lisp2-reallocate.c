#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STACK_MAX 256
#define HEAP_MIN 16
#define HEAP_HEADROOM 1.5

typedef enum {
  OBJ_INT,
  OBJ_PAIR
} ObjectType;

typedef struct sObject {
  ObjectType type;

  // During the sweep phase of garbage collection, this will be non-NULL if the
  // object was reached, otherwise it will be NULL. Before compaction, this
  // will store the address that the object will end up at after compaction.
  // Once garbage collection is done, this is reset to NULL.
  void* moveTo;

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

typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  // The beginning of the contiguous block of memory that objects are allocated
  // from.
  void* heap;

  // Pointer to immediately past the end of the heap.
  void* end;

  // The beginning of the next chunk of memory to be allocated from the heap.
  void* next;
} VM;

void assert(int condition, const char* message) {
  if (!condition) {
    printf("%s\n", message);
    exit(1);
  }
}

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

VM* newVM() {
  VM* vm = malloc(sizeof(VM));
  vm->stackSize = 0;

  vm->heap = malloc(HEAP_MIN);
  vm->end = vm->heap + HEAP_MIN;
  vm->next = vm->heap;

  return vm;
}

void push(VM* vm, Object* value) {
  assert(vm->stackSize < STACK_MAX, "Stack overflow!");
  vm->stack[vm->stackSize++] = value;
}


Object* pop(VM* vm) {
  assert(vm->stackSize > 0, "Stack underflow!");
  return vm->stack[--vm->stackSize];
}

void mark(Object* object) {
  // If already marked, we're done. Check this first to avoid recursing
  // on cycles in the object graph.
  if (object->moveTo) return;

  // Any non-zero pointer indicates the object was reached. For no particular
  // reason, we use the object's own address as the marked value.
  object->moveTo = object;

  if (object->type == OBJ_PAIR) {
    mark(object->head);
    mark(object->tail);
  }
}

void markAll(VM* vm)
{
  for (int i = 0; i < vm->stackSize; i++) {
    mark(vm->stack[i]);
  }
}

size_t calculateNewLocations(VM* vm)
{
  void* from = vm->heap;

  // Calculate the new locations of the objects relative to the heap's current
  // address. When we're done, we may end up reallocating and moving the heap,
  // but these will let us calculate the moved address based on the old heap
  // location.
  void* to = vm->heap;
  while (from < vm->next) {
    Object* object = (Object*)from;
    if (object->moveTo) {
      object->moveTo = to;
      to += sizeof(Object);
    }

    from += sizeof(Object);
  }

  return to - vm->heap;
}

void updateAllObjectPointers(VM* vm, void* oldHeap)
{
  // Walk the heap.
  void* from = vm->heap;
  while (from < vm->next) {
    Object* object = (Object*)from;

    if (object->moveTo) {
      switch (object->type) {
        case OBJ_INT:
          // Nothing to do.
          break;

        case OBJ_PAIR:
          // Calculate the new addresses as an offset from the old heap in case
          // the heap itself moved.
          object->head = vm->heap + (object->head->moveTo - oldHeap);
          object->tail = vm->heap + (object->tail->moveTo - oldHeap);
          break;
      }
    }

    from += sizeof(Object);
  }

  // Fix the stack pointers.
  for (int i = 0; i < vm->stackSize; i++) {
    // Find the object referenced by the stack. We have to do a little
    // arithmetic here because the stack points to the object's old location in
    // oldHeap and the heap may have been reallocated. The *relative* pointer
    // is still valid, so we recalculate the object's address in the new heap
    // based on where it was relative to the beginning of the old heap.
    Object* object = ((void*)vm->stack[i] - oldHeap) + vm->heap;

    // Update the pointer on the stack to point to the object's new compacted
    // location.
    vm->stack[i] = vm->heap + (object->moveTo - oldHeap);
  }
}

void compact(VM* vm, void* oldHeap) {
  void* from = vm->heap;

  while (from < vm->next) {
    Object* object = (Object*)from;
    if (object->moveTo) {
      // Move the object from its old location to its new location relative to
      // the heap's (possibly new) location.
      Object* to = vm->heap + (object->moveTo - oldHeap);
      memmove(to, object, sizeof(Object));

      // Clear the mark.
      to->moveTo = NULL;
    }

    from += sizeof(Object);
  }
}

void gc(VM* vm, size_t additionalSize) {
  markAll(vm);
  size_t liveSize = calculateNewLocations(vm);

  // Grow the heap to ensure we have enough headroom.
  size_t heapSize = liveSize * HEAP_HEADROOM + additionalSize;
  if (heapSize < HEAP_MIN) heapSize = HEAP_MIN;

  void* oldHeap = vm->heap;
  vm->heap = realloc(vm->heap, heapSize);
  vm->end = vm->heap + heapSize;
  vm->next = vm->heap + liveSize;

  updateAllObjectPointers(vm, oldHeap);
  compact(vm, oldHeap);

  printf("%ld live bytes after collection. Heap size %ld.\n",
         vm->next - vm->heap, vm->end - vm->heap);
}

Object* newObject(VM* vm, ObjectType type) {
  if (vm->next + sizeof(Object) > vm->end) gc(vm, sizeof(Object));

  Object* object = (Object*)vm->next;
  vm->next += sizeof(Object);

  object->type = type;
  object->moveTo = NULL;

  return object;
}

void pushInt(VM* vm, int intValue) {
  Object* object = newObject(vm, OBJ_INT);
  object->value = intValue;

  push(vm, object);
}

Object* pushPair(VM* vm) {
  Object* object = newObject(vm, OBJ_PAIR);
  object->tail = pop(vm);
  object->head = pop(vm);

  push(vm, object);
  return object;
}

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

void freeVM(VM *vm) {
  free(vm->heap);
  free(vm);
}

void test1() {
  printf("Test 1: Objects on stack are preserved.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);

  gc(vm, 0);
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

  gc(vm, 0);
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

  gc(vm, 0);
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

  gc(vm, 0);
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
