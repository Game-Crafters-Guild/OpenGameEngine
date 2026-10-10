#version 450

// Selection mask draw — minimal fragment shader.
//
// Outputs 1.0 into the single-channel R8 selection mask. The follow-up
// SelectionOutline composite pass runs a Sobel kernel over this mask to
// produce the screen-space outline.

layout(location = 0) out float oMask;

void main()
{
    oMask = 1.0;
}
