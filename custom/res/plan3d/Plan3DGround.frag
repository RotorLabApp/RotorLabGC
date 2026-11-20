// RotorLab GC Plan view 3D (G64, MG2): known ground shaded (imagery when GC has
// the tiles), unknown ground violet with hatching (PLAN-CONTRACT.md A-3, CL-2).

VARYING vec4 vColor;
VARYING vec2 vUv;
VARYING vec3 vNormal;
VARYING vec3 vWorld;

void MAIN()
{
    if (vColor.a < 0.0) {
        float stripe = step(0.5, fract((vWorld.x + vWorld.z) / hatchSpacing));
        FRAGCOLOR = vec4(mix(vColor.rgb, vColor.rgb * 0.62, stripe), 1.0);
        return;
    }
    vec3 n = normalize(vNormal);
    float l = 0.55 + 0.45 * max(dot(n, normalize(vec3(0.35, 1.0, 0.55))), 0.0);
    vec3 base = vColor.rgb;
    if (useImagery > 0.5) {
        vec4 img = texture(imagery, vec2(vUv.x, 1.0 - vUv.y));
        base = mix(base, img.rgb, img.a);
    }
    FRAGCOLOR = vec4(base * l, 1.0);
}
