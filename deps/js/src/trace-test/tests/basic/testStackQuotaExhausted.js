// Fun.apply ignores arguments past JS_ARGS_LENGTH_MAX = 2^19 - 1024
const numFatArgs = Math.pow(2,19) - 1024;
// Recursion on trace is limited to MAX_CALL_STACK_ENTRIES = 500
const traceDepth = 490;

var trace = true;

function maybeTrace(x) {
    if (!trace)
        eval("");
    if (x <= 0)
        return 0;
    return maybeTrace(x-1);
}

function fatStack() {
    return maybeTrace(traceDepth);
}

// This tests that we conservatively guard against stack space exhaustion
// before entering trace.
exception = false;
try {
    fatStack.apply(null, new Array(numFatArgs));
} catch (e) {
    assertEq(e.toString(), "InternalError: script stack space quota is exhausted");
    exception = true;
}
assertEq(exception, true);
checkStats({traceCompleted:1});

// This tests that, without tracing, we exhaust stack space.
trace = false;
var exception = false;
try {
    fatStack.apply(null, new Array(numFatArgs));
} catch (e) {
    assertEq(e.toString(), "InternalError: script stack space quota is exhausted");
    exception = true;
}
assertEq(exception, true);
