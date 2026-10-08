# RFC 6570 Level 4 URI Template Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement RFC 6570 Level 4 URI Template modifiers (`:len` prefix truncation and `*` explode array expansion/matching) in `mcpkit-c`.

**Architecture:** Extend the internal `parsed_var_expr_t` structure in `src/protocol/uri_template.c` with modifier tagging (`MCP_URI_MOD_NONE`, `MCP_URI_MOD_PREFIX`, `MCP_URI_MOD_EXPLODE`). Implement prefix truncation and multi-element JSON Array formatting during `mcp_uri_template_expand`, and reverse parse exploded path/query/simple patterns into `mcp_json_array_t` structures during `mcp_uri_template_match`.

**Tech Stack:** Pure C99/C23, CMake, CTest, internal JSON object/array API (`mcp_json_value_t`).

---

### Task 1: Parser Syntax & Modifier Validation

**Files:**
- Modify: `src/protocol/uri_template.c:79-115`
- Test: `tests/unit/test_uri_template.c:13-25`

- [ ] **Step 1: Write the failing test**

In `tests/unit/test_uri_template.c`, add negative syntax tests for malformed Level 4 modifiers:

```c
    /* Malformed Level 4 template syntax */
    CHECK(mcp_uri_template_match(ctx, "file:///{var:}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{var:abc}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{var:0}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{var:3*}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{:3}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R test_uri_template --output-on-failure`  
Expected: FAIL (currently `{var:}` or `{var:abc}` are treated as literal variable names without validation).

- [ ] **Step 3: Implement minimal parser validation**

In `src/protocol/uri_template.c`, update `parsed_var_expr_t` and `parse_var_expr`:

```c
typedef enum {
    MCP_URI_MOD_NONE = 0,
    MCP_URI_MOD_PREFIX,
    MCP_URI_MOD_EXPLODE
} mcp_uri_modifier_t;

typedef struct {
    char op;
    const char *name;
    size_t name_len;
    bool is_reserved;
    const char *prefix;
    bool is_query;
    mcp_uri_modifier_t mod;
    size_t prefix_len;
} parsed_var_expr_t;

static bool parse_var_expr(const char *start, const char *end, parsed_var_expr_t *out) {
    if (start >= end) return false;
    char first = *start;
    const char *name_start = start;
    if (first == '+' || first == '#' || first == '/' || first == '.' || first == '?') {
        out->op = first;
        name_start = start + 1;
        out->is_reserved = (first == '+' || first == '#');
        out->is_query = (first == '?');
        if (first == '+') out->prefix = "";
        else if (first == '#') out->prefix = "#";
        else if (first == '/') out->prefix = "/";
        else if (first == '.') out->prefix = ".";
        else if (first == '?') out->prefix = "?";
        else out->prefix = "";
    } else {
        out->op = 0;
        name_start = start;
        out->is_reserved = false;
        out->is_query = false;
        out->prefix = "";
    }

    if (name_start >= end) return false;

    // Scan for modifier (* or :len)
    const char *colon = NULL;
    for (const char *s = name_start; s < end; s++) {
        if (*s == ':') {
            colon = s;
            break;
        }
    }

    out->mod = MCP_URI_MOD_NONE;
    out->prefix_len = 0;

    if (*(end - 1) == '*') {
        if (colon != NULL) return false; // Cannot combine :len and *
        out->mod = MCP_URI_MOD_EXPLODE;
        out->name = name_start;
        out->name_len = (size_t)((end - 1) - name_start);
        if (out->name_len == 0) return false;
    } else if (colon != NULL) {
        if (colon == name_start) return false; // No variable name before ':'
        const char *num_start = colon + 1;
        if (num_start >= end) return false; // Empty length after ':'
        size_t len_val = 0;
        for (const char *s = num_start; s < end; s++) {
            if (*s < '0' || *s > '9') return false;
            len_val = len_val * 10 + (size_t)(*s - '0');
            if (len_val > 999999) return false;
        }
        if (len_val == 0) return false; // :0 is invalid in RFC 6570
        out->mod = MCP_URI_MOD_PREFIX;
        out->prefix_len = len_val;
        out->name = name_start;
        out->name_len = (size_t)(colon - name_start);
    } else {
        out->mod = MCP_URI_MOD_NONE;
        out->name = name_start;
        out->name_len = (size_t)(end - name_start);
    }

    return out->name_len > 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R test_uri_template --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/protocol/uri_template.c tests/unit/test_uri_template.c
git commit -m "feat(uri_template): parse and validate RFC 6570 Level 4 modifier syntax"
```

---

### Task 2: Prefix Modifier (`:len`) Expand & Match

**Files:**
- Modify: `src/protocol/uri_template.c:160-205,340-410`
- Test: `tests/unit/test_uri_template.c:130-160`

- [ ] **Step 1: Write the failing test**

In `tests/unit/test_uri_template.c`, add tests for prefix expansion and matching:

```c
    /* Level 4 prefix modifier {var:len} */
    mcp_json_value_t *pfx_vars = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set_take(ctx, pfx_vars, "word", mcp_json_string_new(ctx, "antigravity")) == MCP_OK);
    CHECK(mcp_json_object_set_take(ctx, pfx_vars, "short", mcp_json_string_new(ctx, "hi")) == MCP_OK);

    char *pfx_expanded = NULL;
    CHECK(mcp_uri_template_expand(ctx, "items/{word:4}", pfx_vars, &pfx_expanded) == MCP_OK);
    CHECK(strcmp(pfx_expanded, "items/anti") == 0);
    mcp_uri_template_free_string(ctx, pfx_expanded);
    pfx_expanded = NULL;

    CHECK(mcp_uri_template_expand(ctx, "items/{short:5}", pfx_vars, &pfx_expanded) == MCP_OK);
    CHECK(strcmp(pfx_expanded, "items/hi") == 0);
    mcp_uri_template_free_string(ctx, pfx_expanded);
    pfx_expanded = NULL;
    mcp_json_destroy(ctx, pfx_vars);

    // Prefix matching
    mcp_json_value_t *pfx_matched = NULL;
    CHECK(mcp_uri_template_match(ctx, "items/{word:4}", "items/anti", &pfx_matched) == MCP_OK);
    CHECK(pfx_matched != NULL);
    const char *w_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, pfx_matched, "word"), &w_val) == MCP_OK);
    CHECK(strcmp(w_val, "anti") == 0);
    mcp_json_destroy(ctx, pfx_matched);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R test_uri_template --output-on-failure`  
Expected: FAIL.

- [ ] **Step 3: Implement prefix modifier logic**

In `src/protocol/uri_template.c`:
1. In `mcp_uri_template_expand`: when `expr.mod == MCP_URI_MOD_PREFIX`, truncate string traversal:
```c
    size_t char_count = 0;
    for (const char *s = val_str; *s != '\0'; s++) {
        if (expr.mod == MCP_URI_MOD_PREFIX && char_count >= expr.prefix_len) {
            break;
        }
        char_count++;
        // ... append encoded character ...
    }
```
2. In `match_step`: when `expr.mod == MCP_URI_MOD_PREFIX`, restrict loop length:
```c
    size_t max_len = u_len;
    if (expr.mod == MCP_URI_MOD_PREFIX && expr.prefix_len < max_len) {
        max_len = expr.prefix_len;
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R test_uri_template --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/protocol/uri_template.c tests/unit/test_uri_template.c
git commit -m "feat(uri_template): implement RFC 6570 Level 4 prefix modifier expand and match"
```

---

### Task 3: Explode Modifier (`*`) Array Forward Expansion

**Files:**
- Modify: `src/protocol/uri_template.c:340-420`
- Test: `tests/unit/test_uri_template.c`

- [ ] **Step 1: Write the failing test**

In `tests/unit/test_uri_template.c`, add tests for array explode expansion:

```c
    /* Level 4 explode modifier {*var} expansion */
    mcp_json_value_t *arr_vars = mcp_json_object_new(ctx);
    mcp_json_value_t *list = mcp_json_array_new(ctx);
    CHECK(mcp_json_array_append(ctx, list, mcp_json_string_new(ctx, "alpha")) == MCP_OK);
    CHECK(mcp_json_array_append(ctx, list, mcp_json_string_new(ctx, "beta")) == MCP_OK);
    CHECK(mcp_json_array_append(ctx, list, mcp_json_string_new(ctx, "gamma")) == MCP_OK);
    CHECK(mcp_json_object_set_take(ctx, arr_vars, "list", list) == MCP_OK);

    char *exp_arr = NULL;
    // Path segment {/list*}
    CHECK(mcp_uri_template_expand(ctx, "repo://root{/list*}", arr_vars, &exp_arr) == MCP_OK);
    CHECK(strcmp(exp_arr, "repo://root/alpha/beta/gamma") == 0);
    mcp_uri_template_free_string(ctx, exp_arr);

    // Form query {?list*}
    CHECK(mcp_uri_template_expand(ctx, "search{?list*}", arr_vars, &exp_arr) == MCP_OK);
    CHECK(strcmp(exp_arr, "search?list=alpha&list=beta&list=gamma") == 0);
    mcp_uri_template_free_string(ctx, exp_arr);

    // Label {.list*}
    CHECK(mcp_uri_template_expand(ctx, "archive{.list*}", arr_vars, &exp_arr) == MCP_OK);
    CHECK(strcmp(exp_arr, "archive.alpha.beta.gamma") == 0);
    mcp_uri_template_free_string(ctx, exp_arr);

    // Simple {list*}
    CHECK(mcp_uri_template_expand(ctx, "tags/{list*}", arr_vars, &exp_arr) == MCP_OK);
    CHECK(strcmp(exp_arr, "tags/alpha,beta,gamma") == 0);
    mcp_uri_template_free_string(ctx, exp_arr);
    mcp_json_destroy(ctx, arr_vars);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R test_uri_template --output-on-failure`  
Expected: FAIL (JSON array currently omitted or treated as NULL string).

- [ ] **Step 3: Implement Array Explode Expansion**

In `src/protocol/uri_template.c`, handle `mcp_json_type(ctx, v) == MCP_JSON_ARRAY`:
- If `expr.mod == MCP_URI_MOD_EXPLODE`:
  - For `expr.op == '?'`: iterate items, write `?` before first and `&` before subsequent, appending `name=encoded_item`.
  - For `expr.op == '/'`: iterate items, write `/` before each item, appending `encoded_item`.
  - For `expr.op == '.'`: iterate items, write `.` before each item, appending `encoded_item`.
  - For `expr.op == 0 || expr.op == '+' || expr.op == '#'`: iterate items, write `,` between items.
- If `v` is empty array (`count == 0`), suppress leading prefix.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R test_uri_template --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/protocol/uri_template.c tests/unit/test_uri_template.c
git commit -m "feat(uri_template): implement RFC 6570 Level 4 explode array expansion"
```

---

### Task 4: Explode Modifier (`*`) Reverse Matching into JSON Array

**Files:**
- Modify: `src/protocol/uri_template.c:160-290`
- Test: `tests/unit/test_uri_template.c`

- [ ] **Step 1: Write the failing test**

In `tests/unit/test_uri_template.c`, add tests for reverse matching exploded paths and query parameters:

```c
    /* Level 4 explode modifier reverse matching */
    mcp_json_value_t *m_arr = NULL;
    CHECK(mcp_uri_template_match(ctx, "repo://root{/path*}", "repo://root/src/protocol/uri", &m_arr) == MCP_OK);
    CHECK(m_arr != NULL);
    const mcp_json_value_t *arr_val = mcp_json_object_get(ctx, m_arr, "path");
    CHECK(arr_val != NULL && mcp_json_type(ctx, arr_val) == MCP_JSON_ARRAY);
    CHECK(mcp_json_array_size(ctx, arr_val) == 3);
    const char *seg0 = NULL, *seg1 = NULL, *seg2 = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_array_get(ctx, arr_val, 0), &seg0) == MCP_OK && strcmp(seg0, "src") == 0);
    CHECK(mcp_json_string_value(ctx, mcp_json_array_get(ctx, arr_val, 1), &seg1) == MCP_OK && strcmp(seg1, "protocol") == 0);
    CHECK(mcp_json_string_value(ctx, mcp_json_array_get(ctx, arr_val, 2), &seg2) == MCP_OK && strcmp(seg2, "uri") == 0);
    mcp_json_destroy(ctx, m_arr);
    m_arr = NULL;

    // Simple explode comma separated
    CHECK(mcp_uri_template_match(ctx, "tags/{list*}", "tags/red,green,blue", &m_arr) == MCP_OK);
    CHECK(m_arr != NULL);
    arr_val = mcp_json_object_get(ctx, m_arr, "list");
    CHECK(arr_val != NULL && mcp_json_type(ctx, arr_val) == MCP_JSON_ARRAY);
    CHECK(mcp_json_array_size(ctx, arr_val) == 3);
    mcp_json_destroy(ctx, m_arr);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R test_uri_template --output-on-failure`  
Expected: FAIL.

- [ ] **Step 3: Implement Explode Reverse Matching**

In `src/protocol/uri_template.c`:
1. In `raw_capture_t`, record `mod` and `op`.
2. In `mcp_uri_template_match`, when building the result JSON object:
   - If `mod == MCP_URI_MOD_EXPLODE`:
     - If `op == '/'`: split along `/`, decode each chunk and create an `mcp_json_array_t`.
     - If `op == '?'`: split along `&`, parse each `name=val` pair, decode each value and create an `mcp_json_array_t`.
     - If `op == 0 || op == '+'`: split along `,`, decode each chunk and create an `mcp_json_array_t`.
     - Set into object via `mcp_json_object_set_take(ctx, obj, name, arr)`.
   - Else populate `mcp_json_string_t` as before.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R test_uri_template --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/protocol/uri_template.c tests/unit/test_uri_template.c
git commit -m "feat(uri_template): implement RFC 6570 Level 4 explode reverse matching into JSON array"
```

---

### Task 5: Documentation & Full Test Suite Verification

**Files:**
- Modify: `include/mcpkit/protocol/uri_template.h:1-25`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update API Documentation & CHANGELOG**

Update `include/mcpkit/protocol/uri_template.h` to declare Level 4 support (`:len` prefix and `*` explode modifier).  
Update `CHANGELOG.md` to document the completed RFC 6570 Level 4 capabilities under the current release notes.

- [ ] **Step 2: Run full project test suite**

Run: `ctest --test-dir build --output-on-failure`  
Expected: 59/59 tests pass (100% pass rate).

- [ ] **Step 3: Commit**

```bash
git add include/mcpkit/protocol/uri_template.h CHANGELOG.md
git commit -m "docs(uri_template): document RFC 6570 Level 4 support in headers and CHANGELOG"
```
