// RotorLab GC Plan view 3D (G64, MG2): instanced markers, a constant size on screen.
// INSTANCE_DATA.x is the marker diameter in px; the built-in #Sphere has radius 50.
// INSTANCE_DATA.yzw is the declared sRGB color (G67 visual M1).
// onTop > 0.5 (G67 M2): the strike markers' second draw. Depth is pressed
// toward the near plane, keeping its order, so a strike under the ground
// stays visible through the terrain.

VARYING vec4 vColor;
VARYING vec3 vNormal;

void MAIN()
{
    vec4 c = INSTANCE_MODEL_MATRIX * vec4(0.0, 0.0, 0.0, 1.0);
    vec4 cc = VIEWPROJECTION_MATRIX * c;
    float worldPerNdcY = cc.w / PROJECTION_MATRIX[1][1];
    float radius = (INSTANCE_DATA.x / viewportSize.y) * worldPerNdcY;
    vec3 world = c.xyz + VERTEX * (radius / 50.0);
    POSITION = VIEWPROJECTION_MATRIX * vec4(world, 1.0);
    if (onTop > 0.5 && POSITION.w > 0.0) {
        float zn = POSITION.z / POSITION.w;
        POSITION.z = (NEAR_CLIP_VALUE + 0.001 * (zn - NEAR_CLIP_VALUE)) * POSITION.w;
    }
    vColor = vec4(INSTANCE_DATA.yzw, 1.0);
    vNormal = NORMAL;
}
