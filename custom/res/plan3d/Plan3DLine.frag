// RotorLab GC Plan view 3D (G64, MG2): line color, dashes for stems over unknown ground (CL-4).

VARYING vec4 vColor;
VARYING vec2 vAlong;

void MAIN()
{
    if (vAlong.y > 0.5 && fract(vAlong.x / dashLength) > 0.5) {
        discard;
    }
    FRAGCOLOR = vColor;
}
