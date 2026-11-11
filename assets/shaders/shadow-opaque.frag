#version 450

// Solid casters need only the fixed-function depth result. Keeping alpha
// testing in a separate shader avoids its fragment resources and discard path.
void main() {}
