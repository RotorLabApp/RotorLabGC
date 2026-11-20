// RotorLab GC Plan view 3D (G64, MG2): screen-space ribbons for the one line mesh.
// Per vertex (Plan3DGeometry.cc): VERTEX this end, NORMAL the other end,
// UV0 (side, end flag), UV1 (distance along, signed width in px; negative = dashed).
// onTop > 0.5 (G67 M2): the strike segments' second draw. Depth is pressed
// toward the near plane, keeping its order, so a strike leg under the ground
// stays visible through the terrain.

VARYING vec4 vColor;
VARYING vec2 vAlong;

void MAIN()
{
    vec4 selfClip = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
    vec4 otherClip = MODELVIEWPROJECTION_MATRIX * vec4(NORMAL, 1.0);
    // Keep the other end in front of the camera so the direction stays right.
    if (otherClip.w < 0.001) {
        float t = (selfClip.w - 0.001) / max(selfClip.w - otherClip.w, 0.000001);
        otherClip = mix(selfClip, otherClip, t);
    }
    vec2 s = selfClip.xy / max(selfClip.w, 0.000001);
    vec2 o = otherClip.xy / max(otherClip.w, 0.000001);
    vec2 dir = (UV0.y > 0.5) ? (s - o) : (o - s);
    dir *= viewportSize;
    float len = length(dir);
    dir = len > 0.000001 ? dir / len : vec2(1.0, 0.0);
    vec2 nrm = vec2(-dir.y, dir.x);
    float w = abs(UV1.y);
    vec2 offsetNdc = nrm * UV0.x * w / viewportSize;
    POSITION = selfClip + vec4(offsetNdc * selfClip.w, 0.0, 0.0);
    if (onTop > 0.5 && POSITION.w > 0.0) {
        float zn = POSITION.z / POSITION.w;
        POSITION.z = (NEAR_CLIP_VALUE + 0.001 * (zn - NEAR_CLIP_VALUE)) * POSITION.w;
    }
    vColor = COLOR;
    vAlong = vec2(UV1.x, UV1.y < 0.0 ? 1.0 : 0.0);
}
