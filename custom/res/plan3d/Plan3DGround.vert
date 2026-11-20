// RotorLab GC Plan view 3D (G64, MG2): ground grid from GC's terrain query.

VARYING vec4 vColor;
VARYING vec2 vUv;
VARYING vec3 vNormal;
VARYING vec3 vWorld;

void MAIN()
{
    vColor = COLOR;
    vUv = UV0;
    vNormal = NORMAL;
    vWorld = VERTEX;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
