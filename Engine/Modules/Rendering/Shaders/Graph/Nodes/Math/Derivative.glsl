// @category Math/Derivative
// @version 1

// @sgnode DDX
// @display "DDX"
// @stage fragment
float SG_DDX(float value) { return dFdx(value); }

// @sgnode DDY
// @display "DDY"
// @stage fragment
float SG_DDY(float value) { return dFdy(value); }

// @sgnode FWidth
// @display "FWidth"
// @stage fragment
float SG_FWidth(float value) { return fwidth(value); }
