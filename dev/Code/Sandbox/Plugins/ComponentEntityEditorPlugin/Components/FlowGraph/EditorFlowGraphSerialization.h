/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/

#include <AzCore/Serialization/DynamicSerializableField.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/Math/Quaternion.h>

#pragma once

namespace LmbrCentral
{
    /**
     * Editor side utility for populating a SerializedFlowGraph structure given an active
     * flowgraph/hypergraph instance.
     */
    void BuildSerializedFlowGraph(IFlowGraphPtr flowGraph, SerializedFlowGraph& graphData);
} // namespace LmbrCentral
