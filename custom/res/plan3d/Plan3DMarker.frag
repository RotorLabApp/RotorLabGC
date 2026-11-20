// RotorLab GC Plan view 3D (G64, MG2): marker shading, unlit but with a simple light for shape.
// vColor is the declared sRGB color from INSTANCE_DATA.yzw, written as is by
// this unshaded material (G67 visual M1). INSTANCE_COLOR is not used: Qt
// stores it linear, which drew breach return #f59e0b as #e85700 and ROI
// #ec4899 as #d41051.

VARYING vec4 vColor;
VARYING vec3 vNormal;

void MAIN()
{
    vec3 n = normalize(vNormal);
    float l = 0.62 + 0.38 * max(dot(n, normalize(vec3(0.35, 1.0, 0.55))), 0.0);
    FRAGCOLOR = vec4(vColor.rgb * l, 1.0);
}
