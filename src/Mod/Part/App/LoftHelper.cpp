// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2025 FreeCAD Project Association                        *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"
#ifndef _PreComp_
#include <algorithm>
#include <sstream>
#endif

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Mod/Part/App/Part2DObject.h>

#include "LoftHelper.h"
#include "FeaturePartCommon.h"

using namespace Part;

std::vector<TopoShape> LoftHelper::extractProfileWires(
    const TopoShape& shape,
    const char* sectionName,
    size_t expectedCount
)
{
    if (shape.isNull()) {
        FC_THROWM(NullShapeException, "Failed to get shape of " << sectionName);
    }

    // Get all wires from the shape
    auto wires = shape.getSubTopoShapes(TopAbs_WIRE);

    // Get free edges (edges not part of any wire) and make wires from them
    auto edges = shape.getSubTopoShapes(TopAbs_EDGE, TopAbs_WIRE);
    if (!edges.empty()) {
        auto extraWires = TopoShape(0).makeElementWires(edges).getSubTopoShapes(TopAbs_WIRE);
        wires.insert(wires.end(), extraWires.begin(), extraWires.end());
    }

    const char* countMismatchMsg
        = "Sections need to have the same amount of wires or vertices as the base section";

    if (!wires.empty()) {
        if (expectedCount && expectedCount != wires.size()) {
            FC_THROWM(Base::CADKernelError, countMismatchMsg);
        }
        return wires;
    }

    // No wires found, check for vertices (for point-to-profile lofts)
    auto vertices = shape.getSubTopoShapes(TopAbs_VERTEX);
    if (vertices.empty()) {
        FC_THROWM(
            Base::CADKernelError,
            "Invalid " << sectionName << " shape, expecting either wires or vertices"
        );
    }
    if (expectedCount && expectedCount != vertices.size()) {
        FC_THROWM(Base::CADKernelError, countMismatchMsg);
    }
    return vertices;
}

std::vector<TopoShape> LoftHelper::extractProfileWiresFromObject(
    App::DocumentObject* obj,
    const std::vector<std::string>& subs,
    const char* sectionName,
    size_t expectedCount
)
{
    // Helper to determine if we should use the entire sketch
    auto useEntireSketch = [](App::DocumentObject* obj, const std::vector<std::string>& subs) {
        // Be smart. If part of a sketch is selected, use the entire sketch unless it is a single
        // vertex - backward compatibility (#16630)
        if (!obj) {
            return false;
        }
        auto subName = subs.empty() ? "" : subs.front();
        return obj->isDerivedFrom<Part::Part2DObject>() && subName.find("Vertex") != 0;
    };

    std::vector<TopoShape> shapes;
    bool useSketch = useEntireSketch(obj, subs);

    if (subs.empty() || std::ranges::find(subs, std::string()) != subs.end() || useSketch) {
        // Use the whole object
        shapes.push_back(
            Feature::getTopoShape(obj, ShapeOption::ResolveLink | ShapeOption::Transform)
        );
        if (shapes.back().isNull()) {
            std::stringstream str;
            str << "Failed to get shape of " << sectionName;
            if (obj) {
                auto doc = obj->getDocument();
                str << " " << App::SubObjectT(obj, "").getSubObjectFullName(doc->getName());
            }
            FC_THROWM(NullShapeException, str.str());
        }
    }
    else {
        // Use specific sub-elements
        for (const auto& sub : subs) {
            shapes.push_back(
                Feature::getTopoShape(
                    obj,
                    ShapeOption::NeedSubElement | ShapeOption::ResolveLink | ShapeOption::Transform,
                    sub.c_str()
                )
            );
            if (shapes.back().isNull()) {
                std::stringstream str;
                str << "Failed to get shape of " << sectionName;
                if (obj) {
                    auto doc = obj->getDocument();
                    App::SubObjectT subObj(obj, sub.c_str());
                    str << " " << subObj.getSubObjectFullName(doc->getName());
                }
                FC_THROWM(NullShapeException, str.str());
            }
        }
    }

    // Combine all shapes into a compound
    auto compound = TopoShape(0).makeElementCompound(
        shapes,
        "",
        TopoShape::SingleShapeCompoundCreationPolicy::returnShape
    );

    // Extract wires from the compound
    return extractProfileWires(compound, sectionName, expectedCount);
}

std::vector<std::vector<TopoShape>> LoftHelper::buildWireSections(
    const std::vector<std::vector<TopoShape>>& sections
)
{
    if (sections.empty()) {
        return {};
    }

    size_t wireCount = sections[0].size();
    std::vector<std::vector<TopoShape>> wireSections;
    wireSections.reserve(wireCount);

    // Initialize with first section's wires
    for (const auto& wire : sections[0]) {
        wireSections.emplace_back(1, wire);
    }

    // Add wires from remaining sections
    for (size_t sectionIdx = 1; sectionIdx < sections.size(); ++sectionIdx) {
        const auto& section = sections[sectionIdx];
        for (size_t wireIdx = 0; wireIdx < section.size(); ++wireIdx) {
            wireSections[wireIdx].push_back(section[wireIdx]);
        }
    }

    return wireSections;
}

TopoShape LoftHelper::makeBackFace(
    const std::vector<std::vector<TopoShape>>& wireSections,
    App::StringHasherRef hasher
)
{
    if (wireSections.empty() || wireSections[0].empty()) {
        return TopoShape();
    }

    // Check if we're dealing with vertices (no face needed)
    if (wireSections[0].back().shapeType() == TopAbs_VERTEX) {
        return TopoShape();
    }

    // Collect the back wire from each wire section
    std::vector<TopoShape> backWires;
    backWires.reserve(wireSections.size());
    for (const auto& sectionWires : wireSections) {
        backWires.push_back(sectionWires.back());
    }

    // Try different face makers in order of preference
    const char* faceMakers[] = {
        "Part::FaceMakerBullseye",
        "Part::FaceMakerCheese",
        "Part::FaceMakerSimple",
        "Part::FaceMakerUnified",
    };

    TopoShape backFace;
    for (size_t i = 0; i < std::size(faceMakers); i++) {
        try {
            backFace = TopoShape(0, hasher).makeElementFace(backWires, nullptr, faceMakers[i]);
            break;
        }
        catch (...) {
            if (i == std::size(faceMakers) - 1) {
                throw;
            }
            continue;
        }
    }

    return backFace;
}
