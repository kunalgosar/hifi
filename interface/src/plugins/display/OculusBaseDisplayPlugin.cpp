//
//  OculusBaseDisplayPlugin.cpp
//
//  Created by Bradley Austin Davis on 2014/04/13.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "OculusBaseDisplayPlugin.h"

#include <ViewFrustum.h>

#include "OculusHelpers.h"


void OculusBaseDisplayPlugin::activate() {
    ovr_for_each_eye([&](ovrEyeType eye) {
        ovrEyeRenderDesc erd = ovrHmd_GetRenderDesc(_hmd, eye, _hmd->MaxEyeFov[eye]);
        ovrMatrix4f ovrPerspectiveProjection =
            ovrMatrix4f_Projection(erd.Fov, DEFAULT_NEAR_CLIP, DEFAULT_FAR_CLIP, ovrProjection_RightHanded);
        _eyeProjections[eye] = toGlm(ovrPerspectiveProjection);
        _eyeOffsets[eye] = erd.HmdToEyeViewOffset;
    });
}

void OculusBaseDisplayPlugin::deactivate() {
}

void OculusBaseDisplayPlugin::preRender() {
    ovrHmd_GetEyePoses(_hmd, _frameIndex, _eyeOffsets, _eyePoses, nullptr);
}

glm::mat4 OculusBaseDisplayPlugin::getProjection(Eye eye, const glm::mat4& baseProjection) const {
    return _eyeProjections[eye];
}

glm::mat4 OculusBaseDisplayPlugin::getModelview(Eye eye, const glm::mat4& baseModelview) const {
    return toGlm(_eyePoses[eye]);
}

