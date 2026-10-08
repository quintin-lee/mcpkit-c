# RFC 6570 Level 4 URI Template Modifiers Design Specification

**Status**: Approved  
**Date**: 2026-10-08  
**Topic**: RFC 6570 Level 4 URI Template Prefix (`:len`) and Explode (`*`) Modifiers in `mcpkit-c`  
**Target Specification**: Model Context Protocol (MCP) 2026-07-28  

---

## 1. Background & Goals

The Model Context Protocol (MCP) 2026-07-28 specification employs RFC 6570 URI Templates for dynamic resource addressing (e.g., `resources/templates/list` and `resources/read`). While `mcpkit-c` already provides Level 1–3 URI template matching and expansion (`{var}`, `{+var}`, `{/var}`, `{#var}`, `{.var}`, `{?var}`), RFC 6570 Level 4 introduces variable modifiers:
1. **Prefix Modifier (`:max-length`)**: Limits string expansion or matching to at most `max-length` characters (e.g., `{id:4}`).
2. **Explode Modifier (`*`)**: Expands composite variables (specifically JSON arrays) into delimited sequences or repeated query parameter pairs, and reverses the match into structured JSON array values (e.g., `{/path*}` or `{?tags*}`).

This specification defines the architecture, data structures, algorithms, error handling, and test matrix required to implement RFC 6570 Level 4 support with zero external dependencies and zero heap leaks in `mcpkit-c`.

---

## 2. Architecture & Data Structures

### 2.1 Modifier Enumeration and Variable Descriptor
In [`src/protocol/uri_template.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/protocol/uri_template.c), the internal variable expression parser is extended with modifier metadata:

```c
typedef enum {
    MCP_URI_MOD_NONE = 0,
    MCP_URI_MOD_PREFIX,   /* Prefix length modifier, e.g. {var:3} */
    MCP_URI_MOD_EXPLODE   /* Explode modifier for arrays, e.g. {list*} or {?tags*} */
} mcp_uri_modifier_t;

typedef struct {
    char op;                    /* Operator: 0, '+', '#', '/', '.', '?' */
    const char *name;           /* Variable name start pointer (without modifier) */
    size_t name_len;            /* Variable name length */
    bool is_reserved;           /* Allow reserved chars (+ or #) */
    const char *prefix;         /* Expansion prefix ("/", "?", "#", ".") */
    bool is_query;              /* Form-style query '?' flag */
    mcp_uri_modifier_t mod;     /* Level 4 modifier type */
    size_t prefix_len;          /* Max character length when mod == MCP_URI_MOD_PREFIX */
} parsed_var_expr_t;
```

### 2.2 Template Syntax Validation & Parsing
During `parse_var_expr`:
1. **Operator Detection**: Leading operator character (`+`, `#`, `/`, `.`, `?`) is extracted.
2. **Modifier Suffix Scanning**:
   - If the expression ends with `*`: set `mod = MCP_URI_MOD_EXPLODE`; variable name ends immediately before `*`.
   - If the expression contains `:`: scan for the colon, verify that all characters following `:` are decimal digits (`0-9`), convert to integer `prefix_len` (must be `> 0` and `< 1000000`), and set `mod = MCP_URI_MOD_PREFIX`.
   - If neither, set `mod = MCP_URI_MOD_NONE`.
3. **Rejection Rules**:
   - An empty prefix length (e.g., `{var:}`) or non-numeric length (e.g., `{var:abc}`) returns syntax error (`MCP_ERR_INVALID_ARGUMENT`).
   - A variable having both modifiers (e.g., `{var:3*}`) returns `MCP_ERR_INVALID_ARGUMENT`.
   - Empty variable names (e.g., `{}` or `{:3}`) are rejected with `MCP_ERR_INVALID_ARGUMENT`.

---

## 3. Forward Expansion (`mcp_uri_template_expand`)

Given a template pattern and a JSON object `variables`:

### 3.1 Prefix Modifier Expansion (`:len`)
* When `mod == MCP_URI_MOD_PREFIX`:
  * Look up the variable as a string.
  * Determine the expansion slice: `take_len = min(strlen(val), prefix_len)`.
  * Percent-encode and append the first `take_len` characters to the output buffer according to the operator's encoding rules.

### 3.2 Explode Modifier Expansion (`*`)
* If the variable is an `MCP_JSON_ARRAY`:
  * **Query Operator (`{?list*}`)**:
    * Each element is expanded with `name=encoded_val`.
    * First element uses `?`, subsequent elements use `&`.
    * Example: `["a", "b"]` -> `?list=a&list=b`.
    * If the array has 0 elements, the `?` prefix is suppressed.
  * **Path Segment Operator (`{/list*}`)**:
    * Each element is prefixed with `/`.
    * Example: `["a", "b"]` -> `/a/b`.
  * **Dot Label Operator (`{.list*}`)**:
    * Each element is prefixed with `.`.
    * Example: `["a", "b"]` -> `.a.b`.
  * **Simple & Reserved (`{list*}` / `{+list*}` / `{#list*}`)**:
    * Elements are joined with `,`.
    * `{#list*}` retains a single leading `#` (e.g., `#a,b`).
    * `{+list*}` allows reserved characters without percent-escaping.
* If the variable is a scalar string/number:
  * Falls back to regular scalar expansion.

---

## 4. Reverse Matching (`mcp_uri_template_match`)

Given a concrete URI and pattern:

### 4.1 Prefix Modifier Matching (`:len`)
* The captured value segment is constrained: `val_len <= prefix_len`.
* If a trailing delimiter or literal matches, the captured string is decoded and placed into `out_variables` as a JSON string.

### 4.2 Explode Modifier Matching (`*`)
When `mod == MCP_URI_MOD_EXPLODE`:
* **Path Segments (`{/list*}`)**:
  * Match consecutive segments separated by `/`.
  * Decode each segment and append to a new `mcp_json_array_t` stored under `name` in `out_variables`.
* **Query Parameters (`{?list*}`)**:
  * Match repeating query parameters `name=val` separated by `&`.
  * Collect decoded values into an `mcp_json_array_t`.
* **Simple Form (`{list*}`)**:
  * Match comma-separated values, split along `,`, decode each and populate an `mcp_json_array_t`.

---

## 5. Memory Safety & Error Handling

1. All string buffers use dynamic geometric reallocation (`cap * 2`) managed by `mcp_context_t` allocator functions.
2. In out-of-memory (`MCP_ERR_NOMEM`) conditions during matching or expanding, all partially allocated strings and JSON structures are recursively destroyed before returning.
3. When `out_variables == NULL`, no JSON objects are allocated during matching, enabling fast boolean pattern checks.

---

## 6. Test Matrix

Update [`tests/unit/test_uri_template.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/tests/unit/test_uri_template.c) to cover:
1. **Prefix Modifier Tests**:
   - Expand `{id:3}` with `"123456"` -> `"123"`.
   - Match `{id:3}` with exact boundary conditions.
2. **Array Explode Tests**:
   - Expand & match `{/segments*}` with `["v1", "v2", "v3"]` <-> `"/v1/v2/v3"`.
   - Expand & match `{?keys*}` with `["k1", "k2"]` <-> `"?keys=k1&keys=k2"`.
   - Expand `{list*}` and `{+list*}` with comma separation.
3. **Negative & Syntax Validation**:
   - Malformed modifier expressions (`{:5}`, `{var:}`, `{var:xyz}`, `{var:5*}`).
