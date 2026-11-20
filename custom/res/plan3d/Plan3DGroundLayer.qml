/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - the ground layer.
 *
 * The ground mesh (terrain from GC's terrain query, Plan3DSceneModel's
 * groundGeometry) with the 2D map's imagery from GC's map tile cache on top.
 * Unknown ground is violet and hatched, never covered by imagery
 * (PLAN-CONTRACT.md A-3, CL-2).
 * Child of the View3D scene.
 *
 ****************************************************************************/

import QtQuick
import QtQuick3D

import RotorLab.Plan3D

Node {
    id: layer

    property var    sceneModel:     null
    property real   cameraDistance: 800

    /// Imagery provider ("Bing Hybrid" ...), and whether its tiles are still coming in.
    readonly property string imagerySource:  imageryData.sourceName
    readonly property bool   imageryLoading: imageryData.loading

    Plan3DGroundImagery {
        id:     imageryData
        area:   layer.sceneModel ? layer.sceneModel.groundArea : []
    }

    Model {
        geometry:   layer.sceneModel ? layer.sceneModel.groundGeometry : null
        materials: [
            CustomMaterial {
                shadingMode:    CustomMaterial.Unshaded
                cullMode:       Material.NoCulling
                vertexShader:   "qrc:/rotorlab/plan3d/Plan3DGround.vert"
                fragmentShader: "qrc:/rotorlab/plan3d/Plan3DGround.frag"
                property real hatchSpacing: Math.max(4, layer.cameraDistance / 60)
                property real useImagery:   imageryData.ready ? 1 : 0
                property TextureInput imagery: TextureInput {
                    texture: Texture {
                        textureData:            imageryData
                        generateMipmaps:        true
                        mipFilter:              Texture.Linear
                        minFilter:              Texture.Linear
                        magFilter:              Texture.Linear
                        tilingModeHorizontal:   Texture.ClampToEdge
                        tilingModeVertical:     Texture.ClampToEdge
                    }
                }
            }
        ]
    }
}
