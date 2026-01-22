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

#ifndef PART_LOFTHELPER_H
#define PART_LOFTHELPER_H

#include <string>
#include <vector>

#include <Mod/Part/PartGlobal.h>
#include "TopoShape.h"

namespace App
{
class DocumentObject;
}

namespace Part
{

/**
 * @brief Helper class for loft operations shared between Part and PartDesign workbenches.
 *
 * This class extracts common loft functionality to enable both Part::Loft and
 * PartDesign::Loft to handle multi-wire profiles (e.g., sketches with concentric
 * circles for creating hollow lofted shapes).
 *
 * The key insight is that multi-wire profiles must be lofted wire-by-wire, with
 * corresponding wires from each section lofted together, then the resulting shells
 * sewn into a solid.
 */
class PartExport LoftHelper
{
public:
    /**
     * @brief Extract wires or vertices from a shape for use as a loft section.
     *
     * This function processes a shape (typically from a sketch or face) and extracts
     * all wires suitable for lofting. It handles:
     * - Compounds (extracts all wires)
     * - Faces (extracts outer and inner wires)
     * - Free edges (converts to wires)
     * - Single wires (returns as-is)
     * - Vertices (for point-to-profile lofts)
     *
     * @param shape The input shape to extract wires from
     * @param sectionName Name for error messages (e.g., "Profile" or "Section")
     * @param expectedCount If non-zero, validates that the extracted count matches.
     *                      Pass 0 for the first section to establish the count.
     * @return Vector of TopoShapes, each being a wire or vertex
     * @throws Base::CADKernelError if shape is invalid or count doesn't match
     */
    static std::vector<TopoShape> extractProfileWires(
        const TopoShape& shape,
        const char* sectionName,
        size_t expectedCount = 0
    );

    /**
     * @brief Extract wires from a document object with optional sub-element selection.
     *
     * This is a higher-level function that gets the shape from a DocumentObject
     * and its sub-elements, then calls extractProfileWires().
     *
     * For sketches, if sub-elements other than vertices are selected, the entire
     * sketch is used (for backward compatibility with #16630).
     *
     * @param obj The document object to get the shape from
     * @param subs Sub-element names (empty means use whole object)
     * @param sectionName Name for error messages
     * @param expectedCount Expected wire/vertex count (0 for first section)
     * @return Vector of TopoShapes, each being a wire or vertex
     */
    static std::vector<TopoShape> extractProfileWiresFromObject(
        App::DocumentObject* obj,
        const std::vector<std::string>& subs,
        const char* sectionName,
        size_t expectedCount = 0
    );

    /**
     * @brief Build a wire correspondence table from multiple sections.
     *
     * Given multiple sections (each with the same number of wires), this builds
     * a 2D table where wiresections[wireIndex][sectionIndex] contains the wire
     * for that position.
     *
     * @param sections Vector of sections, each section being a vector of wires/vertices
     * @return 2D vector: wiresections[wireIndex][sectionIndex]
     */
    static std::vector<std::vector<TopoShape>> buildWireSections(
        const std::vector<std::vector<TopoShape>>& sections
    );

    /**
     * @brief Create faces from the back (last) wires of each wire section.
     *
     * For a solid loft, the back face needs to be created from the last wires
     * of each section. This handles multi-wire profiles by trying different
     * face makers (Bullseye, Cheese, Simple) to create the face.
     *
     * @param wireSections The wire sections from buildWireSections()
     * @param hasher String hasher for element mapping
     * @return The back face, or null shape if wires are vertices
     */
    static TopoShape makeBackFace(
        const std::vector<std::vector<TopoShape>>& wireSections,
        App::StringHasherRef hasher = App::StringHasherRef()
    );
};

}  // namespace Part

#endif  // PART_LOFTHELPER_H
