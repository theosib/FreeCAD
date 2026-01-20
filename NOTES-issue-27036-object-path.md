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

## Implementation Considerations

### Memory Layout Options (if class wrapper desired later)

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

**Recommendation:** Option A with lazy offset computation provides the best balance if a class wrapper is desired.

### Trailing Dot Handling
```cpp
// "Body.Pad." has 3 components: "Body", "Pad", ""
// The empty string represents an empty element name
// This is significant and must be preserved
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

## Recommended Approach: Centralized Path Library

### Philosophy

Rather than creating a new class that changes how paths are stored, the goal should be:

1. **Centralize all path manipulation logic** into a single library
2. **Continue using strings internally** (short-term or long-term)
3. **Provide comprehensive functions** for all path operations
4. **Fix bugs in one place** rather than scattered across modules
5. **Ensure TNP compatibility** - no regressions in topological naming

### Key Insight

The problem isn't that we use strings - it's that every module implements its own path manipulation logic. This leads to:
- Inconsistent handling of edge cases (trailing dots, mapped elements)
- Duplicated bugs across modules
- Different interpretations of path semantics

### Proposed Library Structure

**Location:** `src/Base/SubNamePath.h` and `src/Base/SubNamePath.cpp`

```cpp
namespace Base {
namespace SubNamePath {

// ============================================================
// Core Parsing Functions
// ============================================================

/// Split path into components, correctly handling trailing dots
std::vector<std::string> split(const std::string& path);

/// Join components back into a path string
std::string join(const std::vector<std::string>& components);

/// Join with custom separator (usually ".")
std::string join(const std::vector<std::string>& components, const std::string& sep);

// ============================================================
// Navigation Functions
// ============================================================

/// Get the first component of a path
/// "Part.Body.Pad.Face1" -> "Part"
std::string_view front(const std::string& path);

/// Get the last component of a path
/// "Part.Body.Pad.Face1" -> "Face1"
std::string_view back(const std::string& path);

/// Get path without the first component
/// "Part.Body.Pad.Face1" -> "Body.Pad.Face1"
std::string tail(const std::string& path);

/// Get path without the last component
/// "Part.Body.Pad.Face1" -> "Part.Body.Pad"
std::string parent(const std::string& path);

/// Append a component to a path
/// ("Part.Body", "Pad") -> "Part.Body.Pad"
std::string append(const std::string& path, const std::string& component);

/// Append multiple components
std::string append(const std::string& path, const std::vector<std::string>& components);

/// Get components from index start to end (exclusive)
/// Equivalent to names[start:end] in Python
std::string slice(const std::string& path, size_t start, size_t end = std::string::npos);

// ============================================================
// Element Name Functions (integrates ElementNamingUtils logic)
// ============================================================

/// Check if path ends with an element name (Edge1, Face2, etc.)
bool hasElement(const std::string& path);

/// Get the element name portion
/// "Part.Body.Pad.Face1" -> "Face1"
std::string_view elementName(const std::string& path);

/// Get path without the element name
/// "Part.Body.Pad.Face1" -> "Part.Body.Pad"
std::string withoutElement(const std::string& path);

/// Get element type (Edge, Face, Vertex, Wire, etc.)
/// "Face1" -> "Face"
std::string_view elementType(const std::string& elementName);

/// Get element index
/// "Face1" -> 1
int elementIndex(const std::string& elementName);

/// Check for mapped element (starts with ';')
bool hasMappedElement(const std::string& path);

/// Check for missing element marker ('?')
bool hasMissingElement(const std::string& path);

/// Find where element name starts in path (returns pointer into string)
const char* findElementName(const std::string& path);

// ============================================================
// Link Array / Index Handling
// ============================================================

/// Check if first component is a numeric index (link array)
/// "0.Pad.Face1" -> true
bool hasArrayIndex(const std::string& path);

/// Get the array index if present
/// "0.Pad.Face1" -> 0
int arrayIndex(const std::string& path);

/// Get path without the array index
/// "0.Pad.Face1" -> "Pad.Face1"
std::string withoutArrayIndex(const std::string& path);

// ============================================================
// Validation
// ============================================================

/// Check if path is well-formed
bool isValid(const std::string& path);

/// Check if path is empty or contains only whitespace
bool isEmpty(const std::string& path);

/// Count number of components
size_t componentCount(const std::string& path);

// ============================================================
// Comparison
// ============================================================

/// Check if two paths are equivalent (handles trailing dot normalization)
bool equivalent(const std::string& path1, const std::string& path2);

/// Check if path starts with prefix
bool startsWith(const std::string& path, const std::string& prefix);

/// Check if path ends with suffix
bool endsWith(const std::string& path, const std::string& suffix);

} // namespace SubNamePath
} // namespace Base
```

### Example: Refactoring Common Pattern

**Before (scattered in multiple files):**
```cpp
// DocumentObject.cpp
std::vector<std::string> names = Base::Tools::splitSubName(sub);
if (names.empty()) return plc;
DocumentObject* subObj = getDocument()->getObject(names.front().c_str());
if (!subObj) return plc;
std::vector<std::string> newNames(names.begin() + 1, names.end());
std::string newSub = Base::Tools::joinList(newNames, ".");
return plc * subObj->getPlacementOf(newSub, targetObj);
```

**After (using centralized library):**
```cpp
// DocumentObject.cpp
using namespace Base::SubNamePath;
if (isEmpty(sub)) return plc;
DocumentObject* subObj = getDocument()->getObject(std::string(front(sub)).c_str());
if (!subObj) return plc;
return plc * subObj->getPlacementOf(tail(sub), targetObj);
```

### TNP (Topological Naming Problem) Considerations

The path library must be **TNP-aware** and not introduce regressions:

1. **Preserve Element Map Prefixes**
   - Never strip or modify `;` prefixes
   - `hasMappedElement()` and `findElementName()` must handle mapped names correctly

2. **Handle Dual Naming**
   - Support both old-style (`Face1`) and new-style (`;Face1;:H...`) element names
   - Integrate with existing `ElementNamingUtils` functions

3. **Maintain Path Integrity**
   - Round-trip guarantee: `join(split(path)) == path`
   - Preserve trailing dots exactly
   - Don't lose any path information during manipulation

4. **Test Against TNP Scenarios**
   - Create test suite with mapped element names
   - Test with external geometry references
   - Test with link arrays and sub-assemblies

## Important Design Constraint: Paths Are Dumb Data

### The Rename Problem

A key insight from community discussion: **paths should be dumb objects** that are not observed or updated.

When an object is renamed (e.g., `Pad` → `Pad001`), paths containing that object name become invalid. Currently:
- The **Expression Engine** handles renames by observing and updating expressions
- The **PropertyLink family** handles renames for linked objects

A path library should **NOT** try to handle renames. This is the job of:
- `PropertyLinkSub` and related classes for stored references
- Expression engine for expressions containing paths
- Higher-level code that manages document relationships

### Implications for Library Design

The path library should be:
1. **Pure string manipulation** - No document awareness
2. **Stateless** - No observers, no update mechanisms
3. **Fast and simple** - Just parsing and composing strings

The library provides **tools**, not **managed references**. Code that needs rename-aware paths should use `PropertyLinkSub` or similar.

```cpp
// Path library: dumb string manipulation
std::string newPath = SubNamePath::append("Body", "Pad");  // Just builds "Body.Pad"

// PropertyLinkSub: smart reference with rename tracking
myProperty.setValue(obj, {"Face1"});  // Tracks object, handles renames
```

### What This Library Does NOT Do

- ❌ Track object renames
- ❌ Validate that paths point to existing objects
- ❌ Observe document changes
- ❌ Update stored paths when objects change

### What This Library DOES Do

- ✅ Parse paths into components
- ✅ Compose paths from components
- ✅ Extract element names, types, indices
- ✅ Handle special cases (trailing dots, mapped elements, array indices)
- ✅ Provide consistent behavior across all callers

## Migration Strategy

### Phase 1: Create Library
- Implement `Base::SubNamePath` namespace with all functions
- Write comprehensive unit tests including TNP cases
- Document all edge cases and behaviors

### Phase 2: Add Forwarding in Tools
- Make `Base::Tools::splitSubName` call `SubNamePath::split`
- Make `Base::Tools::joinList` call `SubNamePath::join` for "." separator
- This catches all existing callers automatically

### Phase 3: Direct Migration (Optional)
- Update code to use `SubNamePath` directly for cleaner intent
- New code uses `SubNamePath` exclusively
- Keep `Tools` functions as deprecated aliases

### Phase 4: Extend as Needed
- Add new operations as patterns emerge
- Optimize hot paths based on profiling
- Consider optional class wrapper if type safety becomes valuable

## Benefits

1. **Minimal Risk**
   - Strings remain the storage format
   - Existing code continues to work
   - Changes are incremental and testable

2. **Centralized Bug Fixes**
   - Fix trailing dot handling once
   - Fix element name parsing once
   - All callers benefit automatically

3. **TNP Safe**
   - Library designed with TNP awareness from start
   - Explicit functions for mapped elements
   - Test suite covers TNP scenarios

4. **Gradual Adoption**
   - No big-bang migration required
   - Teams can adopt at their own pace
   - Easy to audit and review

5. **Clear Separation of Concerns**
   - Path library: string manipulation only
   - PropertyLink family: reference management with rename tracking
   - No confusion about responsibilities

## Summary

A centralized path library would:
1. **Consolidate scattered logic** - One place for all path operations
2. **Fix bugs once** - All modules benefit from fixes
3. **Maintain string compatibility** - No storage format changes required
4. **Be TNP-aware** - Designed to handle element naming correctly
5. **Stay dumb** - No rename tracking, that's PropertyLink's job
6. **Enable gradual migration** - Low risk, incremental adoption

The focus should be on **correctness and consolidation** of string manipulation, leaving reference management to the PropertyLink family.
