# Issue #27036: ObjectPath Class Proposal

## Overview

FreeCAD extensively uses `std::string` to represent object paths (e.g., `Part.Part001.Body.Pad.Edge1`). This creates maintenance challenges and error-prone code patterns throughout the codebase.

**Issue:** https://github.com/FreeCAD/FreeCAD/issues/27036

## Current Implementation

### Path Format

```
ObjectPath := ObjectName ('.' ObjectName)* ('.' ElementName)?
ObjectName := Identifier
ElementName := ('Edge' | 'Face' | 'Vertex' | 'Wire' | ...) Digit+
              | MappedElement | TaggedElement

Examples:
  "Part"                      → Object only
  "Part.Body"                 → Two-level object hierarchy
  "Part.Body.Pad"             → Three-level object hierarchy
  "Part.Body.Pad.Face1"       → Object path + element name
  "Body."                     → Trailing dot (valid, empty element)
  "Part.Body.Pad.;Face1"      → Mapped element name
  "Part.Body.?Pad.Face1"      → Missing element marker
```

### Core Utility Functions

**Location:** `src/Base/Tools.h` and `src/Base/Tools.cpp`

```cpp
// Split path into components (Tools.cpp:304-323)
std::vector<std::string> Base::Tools::splitSubName(const std::string& subname)
{
    // Turns 'Part.Part001.Body.Pad.Edge1'
    // Into ['Part', 'Part001', 'Body', 'Pad', 'Edge1']
    std::vector<std::string> subNames;
    std::string subName;
    std::istringstream subNameStream(subname);
    while (std::getline(subNameStream, subName, '.')) {
        subNames.push_back(subName);
    }

    // Handle trailing dots (significant for element references)
    if (!subname.empty() && subname.back() == '.') {
        subNames.push_back("");
    }

    return subNames;
}

// Join components back into path (Tools.cpp:287)
std::string Base::Tools::joinList(const std::vector<std::string>& vec, const std::string& sep);
```

### Element Naming System

**Location:** `src/App/ElementNamingUtils.h` and `src/App/ElementNamingUtils.cpp`

Special prefixes and postfixes for element naming:
```cpp
constexpr const char* ELEMENT_MAP_PREFIX           = ";";      // Mapped elements
constexpr const char* MISSING_PREFIX               = "?";      // Missing elements
constexpr const char* MAPPED_CHILD_ELEMENTS_PREFIX = ";:R";    // Mapped children
constexpr const char* POSTFIX_TAG                  = ";:H";    // Tagged elements
constexpr const char* POSTFIX_EXTERNAL_TAG         = ";:X";    // External geometry
constexpr const char* POSTFIX_CHILD                = ";:C";    // Child element
constexpr const char* POSTFIX_INDEX                = ";:I";    // Array index
constexpr const char* POSTFIX_MOD                  = ";:M";    // Modification
constexpr const char* POSTFIX_MODGEN               = ";:MG";   // Mod + generation
constexpr const char* POSTFIX_DUPLICATE            = ";D";     // Duplicate
```

Key functions:
- `isMappedElement(const char* name)` - Check for ELEMENT_MAP_PREFIX
- `newElementName(const char* name)` - Extract new-style element name
- `oldElementName(const char* name)` - Extract old-style element name
- `noElementName(const char* name)` - Strip all element naming suffixes
- `findElementName(const char* subname)` - Locate element name in path
- `hasMissingElement(const char* subname)` - Detect missing elements

## Usage Patterns in Codebase

### Files Using splitSubName

| File | Lines | Purpose |
|------|-------|---------|
| `src/App/Link.cpp` | 2652, 2782 | Decompose paths for placement resolution |
| `src/App/DocumentObject.cpp` | 1631 | Traverse placement hierarchy |
| `src/Gui/Selection/Selection.cpp` | 376 | Reconstruct paths relative to containers |
| `src/Mod/Assembly/App/AssemblyUtils.cpp` | 472, 530, 668 | Assembly object traversal |
| `src/Mod/Assembly/Gui/ViewProviderAssembly.cpp` | 750 | Selection filtering |
| `src/Mod/Part/Gui/TaskAttacher.cpp` | 476 | Attachment path validation |
| `src/Gui/View3DInventorViewer.cpp` | 3694 | Rotation calculations |

### Common Code Pattern

This pattern appears repeatedly throughout the codebase:

```cpp
// 1. Split path
std::vector<std::string> names = Base::Tools::splitSubName(subname);

// 2. Process first element
auto obj = doc->getObject(names.front().c_str());

// 3. Create remaining path
std::vector<std::string> newNames(names.begin() + 1, names.end());
std::string newSub = Base::Tools::joinList(newNames, ".");

// 4. Recurse or continue
return obj->someMethod(newSub, ...);
```

### Example: Recursive Path Traversal

**DocumentObject.cpp:1622-1646:**
```cpp
Base::Placement DocumentObject::getPlacementOf(const std::string& sub, DocumentObject* targetObj)
{
    std::vector<std::string> names = Base::Tools::splitSubName(sub);

    if (names.empty() || this == targetObj) {
        return plc;
    }

    DocumentObject* subObj = getDocument()->getObject(names.front().c_str());

    if (!subObj) {
        return plc;
    }

    std::vector<std::string> newNames(names.begin() + 1, names.end());
    std::string newSub = Base::Tools::joinList(newNames, ".");

    return plc * subObj->getPlacementOf(newSub, targetObj);
}
```

### Example: Manual Path Building

**Selection.cpp:376-411:**
```cpp
std::vector<std::string> names = Base::Tools::splitSubName(sub);

for (auto& name : names) {
    App::DocumentObject* obj = doc->getObject(name.c_str());
    if (!obj) {
        newSub += name;  // Manual append
        break;
    }

    if (objPassed) {
        if (!newRootObj) {
            newRootObj = obj;
        }
        else {
            newSub += name + ".";  // Manual concatenation with literal dot
        }
    }
}
```

## Problems with Current Approach

### 1. Lack of Type Safety
- Generic strings don't signal structured path content
- Easy to create malformed paths
- No compile-time validation

### 2. Repeated Parsing
- Functions split strings, process, then rejoin
- Pattern appears in Link.cpp, DocumentObject.cpp, AssemblyUtils.cpp, etc.
- Inefficient for deep hierarchies

### 3. Error-Prone Manual Operations
- Manual string concatenation with `"."` throughout
- Manual vector slicing using iterators
- Off-by-one errors easy to introduce

### 4. Trailing Dot Semantics
- Trailing dots are significant (indicate element reference position)
- Must be handled explicitly
- Easy to forget this special case

### 5. No Standard Path Operations
- No `parent()` operation
- No `append()` operation
- No component iteration
- Different modules implement their own helpers

### 6. Element Name Complexity
- Dual naming system (old vs new) for backwards compatibility
- Multiple prefix/postfix markers
- Complex logic scattered across functions

### 7. Link Group Special Cases
- Link arrays add integer indices as first path component
- Example: `"1.pad.face3"` where `1` is array index
- Requires type-specific handling

## Proposed Solution

### Base::ObjectPath Class

A new class modeled after `std::filesystem::path`:

```cpp
namespace Base {

class ObjectPath {
public:
    // Construction
    ObjectPath() = default;
    ObjectPath(const std::string& path);      // Implicit conversion
    ObjectPath(const char* path);             // Implicit conversion
    ObjectPath(std::string_view path);

    // Conversion back to string
    std::string toString() const;
    operator std::string() const { return toString(); }
    const char* c_str() const;

    // Path components
    std::vector<std::string_view> components() const;
    size_t size() const;                      // Number of components
    bool empty() const;

    // Navigation
    std::string_view front() const;           // First component
    std::string_view back() const;            // Last component (may be element)
    ObjectPath parent() const;                // Path without last component
    ObjectPath tail() const;                  // Path without first component

    // Element handling
    bool hasElement() const;                  // Ends with element name?
    std::string_view elementName() const;     // Get element name (Edge1, Face2, etc.)
    std::string_view elementType() const;     // Get element type (Edge, Face, etc.)
    int elementIndex() const;                 // Get element index (1, 2, etc.)
    ObjectPath withoutElement() const;        // Path with element stripped
    ObjectPath withElement(std::string_view elem) const;

    // Special element markers
    bool hasMappedElement() const;            // Has ';' prefix
    bool hasMissingElement() const;           // Has '?' marker

    // Composition
    ObjectPath operator/(const ObjectPath& other) const;  // Append
    ObjectPath operator/(std::string_view component) const;
    ObjectPath& operator/=(const ObjectPath& other);
    ObjectPath& operator/=(std::string_view component);

    // Comparison
    bool operator==(const ObjectPath& other) const;
    bool operator!=(const ObjectPath& other) const;
    bool operator<(const ObjectPath& other) const;  // For use in maps

    // Iteration
    class iterator;
    iterator begin() const;
    iterator end() const;

    // Validation
    bool isValid() const;

private:
    std::string m_path;
    // Optional: cached component boundaries for efficiency
    mutable std::vector<size_t> m_componentOffsets;
    mutable bool m_parsed = false;
};

} // namespace Base
```

### Usage Examples

```cpp
// Construction (implicit from string)
ObjectPath path = "Part.Body.Pad.Face1";

// Navigation
ObjectPath parent = path.parent();        // "Part.Body.Pad"
ObjectPath tail = path.tail();            // "Body.Pad.Face1"
auto first = path.front();                // "Part"

// Element handling
if (path.hasElement()) {
    auto elem = path.elementName();       // "Face1"
    auto type = path.elementType();       // "Face"
    int idx = path.elementIndex();        // 1
}

// Composition
ObjectPath newPath = parent / "Pocket" / "Edge3";  // "Part.Body.Pocket.Edge3"

// Iteration
for (auto component : path) {
    // Process each component
}

// Use with existing APIs (implicit conversion)
auto obj = doc->getObject(path.front());  // Works via c_str()
```

### Document API Addition

```cpp
class Document {
public:
    // New method
    DocumentObject* getByPath(const Base::ObjectPath& path);

    // Could also add:
    std::pair<DocumentObject*, std::string> resolvePath(const Base::ObjectPath& path);
};
```

## Migration Strategy

### Phase 1: Add ObjectPath Class
- Implement `Base::ObjectPath` in `src/Base/ObjectPath.h` and `src/Base/ObjectPath.cpp`
- Implicit conversions allow gradual adoption
- No changes to existing code required

### Phase 2: Update Utility Functions
- Add ObjectPath overloads to frequently used functions
- Keep string versions for compatibility

### Phase 3: Gradual Migration
- Update high-traffic code paths first (Selection, Link, Assembly)
- Use ObjectPath in new code
- Deprecate string-based patterns over time

### Phase 4: Optimization
- Cache parsed components for repeated access
- Avoid allocations for common operations
- Profile and optimize hot paths

## Implementation Considerations

### Memory Layout Options

**Option A: Store original string + cached offsets**
```cpp
std::string m_path;
std::vector<size_t> m_offsets;  // Lazily computed
```
- Pro: Single allocation for path, efficient string conversion
- Con: Extra allocation for offsets if iteration needed

**Option B: Store components directly**
```cpp
std::vector<std::string> m_components;
mutable std::string m_cachedPath;
```
- Pro: Fast iteration and component access
- Con: More allocations, need to rebuild string for c_str()

**Option C: Small string optimization**
```cpp
union {
    char m_small[32];
    struct { char* ptr; size_t len; size_t cap; } m_large;
};
```
- Pro: No allocation for short paths
- Con: Complexity

**Recommendation:** Option A with lazy offset computation provides the best balance.

### Thread Safety
- ObjectPath should be value type (copyable, movable)
- Const methods should be thread-safe
- Mutable cached data needs synchronization or per-call computation

### Trailing Dot Handling
```cpp
ObjectPath path = "Body.Pad.";
path.hasElement();     // true (empty element)
path.elementName();    // "" (empty string)
path.back();           // ""
path.size();           // 3 components: "Body", "Pad", ""
```

## Related Work

### Existing Abstractions to Consider
- `SubObjectT` class (in PropertyLinks.h) - structured path representation
- `ElementNamePair` struct - dual naming for compatibility
- `PropertyXLinkSub` - property system managing subname references

### Similar Patterns in Other Projects
- `std::filesystem::path` - C++17 path abstraction
- Qt's `QDir`/`QFileInfo` - file path handling
- Boost.Filesystem - cross-platform path operations

## Summary

A dedicated `ObjectPath` class would:
1. **Reduce bugs** - Type safety prevents malformed paths
2. **Improve readability** - Clear intent with named operations
3. **Eliminate duplication** - Standard way to manipulate paths
4. **Improve performance** - Avoid repeated parsing
5. **Simplify code** - Replace verbose split/join patterns

The implicit conversion from `std::string` allows gradual adoption without breaking existing code.
