# Issue #10760: PartDesign Fillet Crash Research

## Overview

FreeCAD crashes when applying a fillet to certain edges. The crash occurs in OCCT (Open Cascade Technology) library functions, specifically `TopLoc_Location::Multiplied()` and `BRep_Tool::Surface()`.

**Issue:** https://github.com/FreeCAD/FreeCAD/issues/10760

## Symptoms

- Application crashes (segmentation fault) when clicking "add fillet"
- Windows: "Access violation" logged repeatedly
- Linux/macOS: Segfault in OCCT libraries
- Affects multiple OS versions (macOS 14.5, Windows 10/11, Ubuntu 22.04)
- Related issues: #6625, #8086

## Code Flow Analysis

### Fillet Execution Path

```
PartDesign::Fillet::execute()                    [FeatureFillet.cpp:75]
    │
    ├── getBaseTopoShape()                       [Feature.cpp:379]
    │       Returns shape from BaseObject->Shape.getShape()
    │
    ├── baseShape.setTransform(Base::Matrix4D()) [FeatureFillet.cpp:89]
    │       Sets identity transform (attempts to normalize location)
    │
    ├── getContinuousEdges(baseShape)            [FeatureDressUp.cpp:186]
    │       │
    │       ├── shape.getSubShape(ref.c_str())   [line 215]
    │       │       Gets edge by reference string
    │       │
    │       └── BRep_Tool::Continuity(edge, face1, face2)  [line 204]
    │               Checks C0 continuity - FIRST OCCT CALL
    │
    └── shape.makeElementFillet(baseShape, edges, radius, radius)  [line 117]
            │
            └── TopoShape::makeElementFillet()   [TopoShapeExpansion.cpp:4078]
                    │
                    ├── BRepFilletAPI_MakeFillet mkFillet(shape.getShape())  [line 4096]
                    │       Creates OCCT fillet builder
                    │
                    └── mkFillet.Add(radius1, radius2, TopoDS::Edge(edge))  [line 4105]
                            CRASH POINT - OCCT accesses edge surface/location
```

### Key Files

| File | Purpose | Critical Lines |
|------|---------|----------------|
| `src/Mod/PartDesign/App/FeatureFillet.cpp` | Main fillet execution | 75-164 |
| `src/Mod/PartDesign/App/FeatureDressUp.cpp` | Edge selection & continuity check | 186-236 |
| `src/Mod/PartDesign/App/Feature.cpp` | Base shape retrieval | 379-418 |
| `src/Mod/Part/App/TopoShapeExpansion.cpp` | OCCT fillet binding | 4078-4108 |

## Current Protection Mechanisms

### Signal Handler (Linux Only)

```cpp
// FeatureFillet.cpp:112-115
#if defined(__GNUC__) && defined(FC_OS_LINUX)
    Base::SignalException se;  // Converts SIGSEGV to exception
#endif
```

**Implementation (Exception.cpp:597-622):**
- Creates a signal handler for SIGSEGV
- Throws `std::runtime_error("throw_signal")` on segfault
- Only works on Linux with GCC
- **Gap: No protection on Windows or macOS**

### Exception Handling

```cpp
// FeatureFillet.cpp:149-163
catch (Base::Exception& e) {
    return new App::DocumentObjectExecReturn(e.what());
}
catch (Standard_Failure& e) {
    return new App::DocumentObjectExecReturn(e.GetMessageString());
}
catch (...) {
    return new App::DocumentObjectExecReturn(
        "Fillet operation failed. The selected edges may contain geometry..."
    );
}
```

### Validation Checks

**In FeatureFillet.cpp:**
```cpp
// Line 93-97: Check edges not empty
if (edges.empty()) {
    return new App::DocumentObjectExecReturn("Fillet not possible on selected shapes");
}

// Line 101-105: Check radius > 0
if (radius <= 0) {
    return new App::DocumentObjectExecReturn("Fillet radius must be greater than zero");
}

// Line 118-122: Check result not null
if (shape.isNull()) {
    return new App::DocumentObjectExecReturn("Resulting shape is null");
}

// Line 126: Validity check
if (!BRepAlgo::IsValid(aLarg, shape.getShape(), ...)) {
    // Adjust tolerances
}
```

**In TopoShapeExpansion.cpp (makeElementFillet):**
```cpp
// Line 4089-4091: Null shape check
if (shape.isNull()) {
    FC_THROWM(NullShapeException, "Null shape");
}

// Line 4093-4095: Empty edges check
if (edges.empty()) {
    FC_THROWM(NullShapeException, "Null input shape");
}

// Line 4098-4100: Null edge check
if (e.isNull()) {
    FC_THROWM(NullShapeException, "Null input shape");
}

// Line 4102-4103: Edge belongs to shape
if (!shape.findShape(edge)) {
    FC_THROWM(Base::CADKernelError, "edge does not belong to the shape");
}
```

## Root Cause Analysis

### Why OCCT Crashes

The crash in `TopLoc_Location::Multiplied()` and `BRep_Tool::Surface()` suggests:

1. **Invalid Location References**: The shape or edge has a `TopLoc_Location` (placement/transform) with invalid or dangling references

2. **Corrupted Geometry**: The edge's underlying surface geometry is invalid or inaccessible

3. **Transform Chain Issues**: Complex transform chains from linked/external geometry may have inconsistent states

### The `setTransform(Base::Matrix4D())` Attempt

```cpp
// FeatureFillet.cpp:89
baseShape.setTransform(Base::Matrix4D());
```

This attempts to normalize the shape's location to identity, but:
- May not fully unwind complex location chains
- Doesn't validate the underlying geometry is accessible
- Doesn't fix already-corrupted location references in sub-shapes (edges)

### Edge Selection Path

```cpp
// FeatureDressUp.cpp:215
subshape = shape.getSubShape(ref.c_str(), true);
```

The edge is obtained from the base shape by reference string. If the base shape has complex/invalid locations, the edge inherits these problems.

## Missing Validations

1. **No Location Validation**
   - No check that edge's location is valid/resolvable
   - No check for null location datum references

2. **No Surface Accessibility Check**
   - No test that `BRep_Tool::Surface(edge)` succeeds before fillet
   - This is exactly where OCCT crashes

3. **No Cross-Platform Crash Protection**
   - `SignalException` only works on Linux/GCC
   - Windows and macOS users get hard crashes

4. **No Pre-Fillet Geometry Validation**
   - Could test each edge with OCCT before committing to fillet
   - BRepCheck_Analyzer could validate edge geometry

## Potential Fixes

### Option 1: Pre-validate Edge Geometry

Add validation before calling `BRepFilletAPI_MakeFillet`:

```cpp
// Before mkFillet.Add()
for (auto& e : edges) {
    const TopoDS_Edge& edge = TopoDS::Edge(e.getShape());

    // Check edge has valid surface
    TopoDS_Face face;
    try {
        // Find a face containing this edge
        TopExp_Explorer exp(shape.getShape(), TopAbs_FACE);
        for (; exp.More(); exp.Next()) {
            for (TopExp_Explorer edgeExp(exp.Current(), TopAbs_EDGE); edgeExp.More(); edgeExp.Next()) {
                if (edgeExp.Current().IsSame(edge)) {
                    face = TopoDS::Face(exp.Current());
                    break;
                }
            }
            if (!face.IsNull()) break;
        }

        // Try to access the surface - this is what crashes
        if (!face.IsNull()) {
            Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
            if (surf.IsNull()) {
                FC_THROWM(Base::CADKernelError, "Edge face has invalid surface");
            }
        }
    }
    catch (Standard_Failure& e) {
        FC_THROWM(Base::CADKernelError, "Edge geometry validation failed: " << e.GetMessageString());
    }
}
```

### Option 2: Deep Copy Shape Before Fillet

Force a complete reconstruction of the shape geometry:

```cpp
// Before fillet operation
BRepBuilderAPI_Copy copier(baseShape.getShape(), Standard_True, Standard_False);
TopoDS_Shape cleanShape = copier.Shape();
// Use cleanShape for fillet
```

This creates a new shape with fresh location data.

### Option 3: Use BRepCheck_Analyzer

```cpp
BRepCheck_Analyzer analyzer(shape.getShape());
if (!analyzer.IsValid()) {
    // Log specific problems
    for (TopExp_Explorer exp(shape.getShape(), TopAbs_EDGE); exp.More(); exp.Next()) {
        BRepCheck_ListOfStatus status;
        analyzer.Result(exp.Current(), status);
        // Check status for issues
    }
    FC_THROWM(Base::CADKernelError, "Shape has invalid geometry");
}
```

### Option 4: Cross-Platform Crash Protection

Extend `SignalException` to work on Windows and macOS:

**Windows:**
```cpp
#ifdef _WIN32
// Use Structured Exception Handling (SEH)
__try {
    // OCCT calls
}
__except(EXCEPTION_EXECUTE_HANDLER) {
    throw std::runtime_error("Access violation in fillet operation");
}
#endif
```

**macOS:**
```cpp
#ifdef __APPLE__
// Similar signal handler approach as Linux
// Or use mach exception handlers
#endif
```

### Option 5: Validate Location Chain

```cpp
bool validateLocation(const TopoDS_Shape& shape) {
    TopLoc_Location loc = shape.Location();
    while (!loc.IsIdentity()) {
        // Check datum is valid
        if (loc.FirstDatum().IsNull()) {
            return false;
        }
        loc = loc.NextLocation();
    }
    return true;
}
```

## Related Issues

- **#6625**: Similar fillet crash (may be related)
- **#8086**: Another fillet stability issue
- **#15663**: App::Link external geometry (we fixed this - may reduce fillet crashes)

## Reproduction

1. Open the `crash.zip` file from the issue
2. Select the green edge shown in the screenshot
3. Click "add fillet"
4. FreeCAD crashes

## Recommendations

1. **Short-term**: Add pre-validation of edge geometry before fillet (Option 1)
2. **Medium-term**: Extend crash protection to Windows/macOS (Option 4)
3. **Long-term**: Investigate why shapes end up with invalid locations and fix upstream

## Files to Modify

| File | Change |
|------|--------|
| `src/Mod/Part/App/TopoShapeExpansion.cpp` | Add edge validation in `makeElementFillet()` |
| `src/Base/Exception.h` | Extend `SignalException` for Windows/macOS |
| `src/Base/Exception.cpp` | Implement cross-platform signal handling |

## Testing Notes

- Need test file from issue (crash.zip)
- Should test on all platforms (Linux, Windows, macOS)
- Verify that valid fillets still work after adding validation
- Check performance impact of validation
