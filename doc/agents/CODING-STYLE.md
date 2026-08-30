# NXU source style

NXU keeps short declarations, assignments, calls, and conditions on one line when they remain readable.

Preferred:

```c
static const uint32_t value = HELLO_WORLD;
uint64_t lower_vbar = arm64_read_vector_base();
```

Avoid splitting a trivial assignment solely because an automatic formatter can do so:

```c
static const uint32_t value =
    HELLO_WORLD;
```

Independent control-flow blocks get visual separation:

```c
if (first_condition) {
    handle_first();
}

if (second_condition) {
    handle_second();
}
```

Use `/* ... */` comments for implementation intent, ownership boundaries, invariants, and non-obvious hardware behavior. Comments should explain why a block exists rather than narrating every statement.
