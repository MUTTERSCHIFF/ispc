
export uniform int width() { return programCount; }

export void f_fu(uniform float RET[], uniform float aFOO[], uniform float b) {
    int16 a = aFOO[programIndex]; 
    int16 rot = rotate(a, -1);
    RET[programIndex] = rot;
}

export void result(uniform float RET[]) {
    RET[programIndex] = 1 + (programIndex + programCount - 1) % programCount;
}
