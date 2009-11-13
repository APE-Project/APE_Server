// tests...

var nsID = Components.ID;
var NS_ISUPPORTS_IID    = new nsID("{00000000-0000-0000-c000-000000000046}");
var NS_ITESTXPC_FOO_IID = new nsID("{159E36D0-991E-11d2-AC3F-00C09300144B}");


var baz = foo.QueryInterface(NS_ITESTXPC_FOO_IID);
print("baz = "+baz);
print("distinct wrapper test "+ (foo != baz ? "passed" : "failed"));
var baz2 = foo.QueryInterface(NS_ITESTXPC_FOO_IID);
print("shared wrapper test "+ (baz == baz2 ? "passed" : "failed"));
print("root wrapper identity test "+
    (foo.QueryInterface(NS_ISUPPORTS_IID) ==
     baz.QueryInterface(NS_ISUPPORTS_IID) ?
        "passed" : "failed"));

print("foo = "+foo);
foo.toString = new Function("return 'foo toString called';")
foo.toStr = new Function("return 'foo toStr called';")
print("foo = "+foo);
print("foo.toString() = "+foo.toString());
print("foo.toStr() = "+foo.toStr());
print("foo.five = "+ foo.five);
print("foo.six = "+ foo.six);
print("foo.bogus = "+ foo.bogus);
print("setting bogus explicitly to '5'...");
foo.bogus = 5;
print("foo.bogus = "+ foo.bogus);
print("foo.Test(10,20) returned: "+foo.Test(10,20));

function _Test(p1, p2)
{
    print("test called in JS with p1 = "+p1+" and p2 = "+p2);
    return p1+p2;
}

function _QI(iid)
{
    print("QueryInterface called in JS with iid = "+iid);
    return  this;
}

print("creating bar");
bar = new Object();
bar.Test = _Test;
bar.QueryInterface = _QI;
// this 'bar' object is accessed from native code after this script is run

var some_string = "some string"
foo.Foo = some_string;
print("property set/get: "+ (foo.Foo == some_string ? "passed" : "FAILED"));

print("foo properties:");
for(i in foo)
    print("  foo."+i+" = "+foo[i]);

print("Components = "+Components);
print("Components properties:");
for(i in Components)
    print("  Components."+i+" = "+Components[i]);


/***************************************************************************/
print(".......................................");
print("echo tests...");

var receiver = new Object();
receiver.SetReceiver = function() {};
receiver.SendOneString = function(str) {receiver_results[0] = str;};
receiver.SendManyTypes = function()
    {
        for(var i = 0; i < arguments.length; i++)
            receiver_results[i] = arguments[i];
    };

echo.SetReceiver(receiver);

////////////////////
// SendOneString

var test_string = "some string";
var receiver_results = new Object();
echo.SendOneString(test_string);
print("SendOneString - "+(
       receiver_results[0] == test_string
       ? "passed" : "failed"));

////////////////////
// In2OutOneInt

print("In2OutOneInt - "+(
       echo.In2OutOneInt(102) == 102
       ? "passed" : "failed"));

////////////////////
// In2OutAddTwoInts

var in_out_results1 = new Object();
var in_out_results2 = new Object();
var in_out_results =
        echo.In2OutAddTwoInts(123, 55, in_out_results1, in_out_results2);
print("In2OutAddTwoInts - "+(
       in_out_results1.value == 123 &&
       in_out_results2.value ==  55 &&
       in_out_results      == 178
       ? "passed" : "failed"));

var test_string2 = "some other string";
print("In2OutOneString - "+(
       echo.In2OutOneString(test_string2) == test_string2 &&
       echo.In2OutOneString(echo.In2OutOneString(test_string2)) == test_string2
       ? "passed" : "failed"));


////////////////////
// SendManyTypes

var receiver_results = new Object();
var send_params = [1,-2,-3,-102020,2,4,6,1023,1.5,2.000008,true,'a','b',NS_ITESTXPC_FOO_IID,"a string","another string"];
echo.SendManyTypes(send_params[0],
                   send_params[1],
                   send_params[2],
                   send_params[3],
                   send_params[4],
                   send_params[5],
                   send_params[6],
                   send_params[7],
                   send_params[8],
                   send_params[9],
                   send_params[10],
                   send_params[11],
                   send_params[12],
                   send_params[13],
                   send_params[14],
                   send_params[15]);

var all_ok = true;
for(i = 0; i < 16; i++) {
    if(((""+receiver_results[i]).toLowerCase()) !=
        ((""+send_params[i]).toLowerCase())) {
        if(all_ok)
            print("SendManyTypes - failed...");
        all_ok = false;
        print("    param number "+i+" diff: "+send_params[i]+" -> "+receiver_results[i])
    }
}
if(all_ok)
    print("SendManyTypes - passed");

////////////////////
// SendInOutManyTypes

var receiver_results = new Object();
var send_params   = [1,-2,-3,-102020,2,4,6,1023,1.5,2.000008,true,'a','b',NS_ITESTXPC_FOO_IID,"a string","another string"];
var resend_params = [2,-3,-7,-10220,18,14,16,123,2.5,8.000008,false,'z','l',NS_ISUPPORTS_IID,"foo string","yet another string"];

receiver.SendInOutManyTypes = function()
    {
        for(var i = 0; i < arguments.length; i++) {
            receiver_results[i] = arguments[i].value;
            arguments[i].value = resend_params[i];
        }
    };

var inout_params = [{value:send_params[0] },
                    {value:send_params[1] },
                    {value:send_params[2] },
                    {value:send_params[3] },
                    {value:send_params[4] },
                    {value:send_params[5] },
                    {value:send_params[6] },
                    {value:send_params[7] },
                    {value:send_params[8] },
                    {value:send_params[9] },
                    {value:send_params[10]},
                    {value:send_params[11]},
                    {value:send_params[12]},
                    {value:send_params[13]},
                    {value:send_params[14]},
                    {value:send_params[15]}];

echo.SendInOutManyTypes(inout_params[0] ,
                        inout_params[1] ,
                        inout_params[2] ,
                        inout_params[3] ,
                        inout_params[4] ,
                        inout_params[5] ,
                        inout_params[6] ,
                        inout_params[7] ,
                        inout_params[8] ,
                        inout_params[9] ,
                        inout_params[10],
                        inout_params[11],
                        inout_params[12],
                        inout_params[13],
                        inout_params[14],
                        inout_params[15]);

var all_ok = true;
for(i = 0; i < 16; i++) {
    if(((""+receiver_results[i]).toLowerCase()) !=
        ((""+send_params[i]).toLowerCase())) {
        if(all_ok)
            print("SendInOutManyTypes - failed...");
        all_ok = false;
        print("    sent param number "+i+" diff: "+send_params[i]+" -> "+receiver_results[i]);
    }
}

for(i = 0; i < 16; i++) {
    if(((""+resend_params[i]).toLowerCase()) !=
        ((""+inout_params[i].value).toLowerCase())) {
        if(all_ok)
            print("SendInOutManyTypes - failed...");
        all_ok = false;
        print("    resent param number "+i+" diff: "+resend_params[i]+" -> "+inout_params[i].value);
    }
}

if(all_ok)
    print("SendInOutManyTypes - passed");

////////////////////
// check exceptions on xpcom error code

try {
    echo.ReturnCode(0);
    print("ReturnCode(0) - passed");
}
catch(e) {
    print("ReturnCode(0) exception text: "+e+" - failed");
}

try {
    echo.ReturnCode(-1);
    print("ReturnCode(-1) - failed");
}
catch(e) {
//    print("ReturnCode(-1) exception text: "+e+" - passed");
    print("ReturnCode(-1) - passed");
}

var all_ok = true;

echo.ReturnCode_NS_OK()
var lastResult = Components.lastResult;
if(Components.results.NS_OK != lastResult) {
    all_ok = false;
    print("expected: NS_OK = "+Components.results.NS_OK+"  got: "+lastResult);
}

try {
    echo.ReturnCode_NS_ERROR_NULL_POINTER()
    all_ok = false;
} catch(e) {
    var lastResult = Components.lastResult;
    if(e.result != lastResult) {
        all_ok = false;
        print("expected: NS_ERROR_NULL_POINTER = "+Components.results.NS_ERROR_NULL_POINTER+"  e.result was: "+e.result+"  Components.lastResult: "+lastResult);
        
    }
    if(Components.results.NS_ERROR_NULL_POINTER != lastResult) {
        all_ok = false;
        print("expected: NS_ERROR_NULL_POINTER = "+Components.results.NS_ERROR_NULL_POINTER+"  got: "+lastResult);
    }
}

try {
    echo.ReturnCode_NS_ERROR_UNEXPECTED()
    all_ok = false;
} catch(e) {
    var lastResult = Components.lastResult;
    if(e.result != lastResult) {
        all_ok = false;
        print("expected: NS_ERROR_UNEXPECTED = "+Components.results.NS_ERROR_UNEXPECTED+"  e.result was: "+e.result+"  Components.lastResult: "+lastResult);
        
    }
    if(Components.results.NS_ERROR_UNEXPECTED != lastResult) {
        all_ok = false;
        print("expected: NS_ERROR_UNEXPECTED = "+Components.results.NS_ERROR_UNEXPECTED+"  got: "+lastResult);
    }
}

try {
    echo.ReturnCode_NS_ERROR_OUT_OF_MEMORY()
    all_ok = false;
} catch(e) {
    var lastResult = Components.lastResult;
    if(e.result != lastResult) {
        all_ok = false;
        print("expected: NS_ERROR_OUT_OF_MEMORY = "+Components.results.NS_ERROR_OUT_OF_MEMORY+"  e.result was: "+e.result+"  Components.lastResult: "+lastResult);
        
    }
    if(Components.results.NS_ERROR_OUT_OF_MEMORY != lastResult) {
        all_ok = false;
        print("expected: NS_ERROR_OUT_OF_MEMORY = "+Components.results.NS_ERROR_OUT_OF_MEMORY+"  got: "+lastResult);
    }
}

print("Components.lastResult test -  "+ (all_ok ? "passed" : "failed") );

////////////////////
// check exceptions on too few args

try {
    echo.ReturnCode();  // supposed to have one arg
    print("Too few args test - failed");
}
catch(e) {
//    print("Too few args test -- exception text: "+e+" - passed");
    print("Too few args test - passed");
}

////////////////////
// check exceptions on can't convert

// XXX this is bad test since null is now convertable.
/*
try {
    echo.SetReceiver(null);
//    print("Can't convert arg to Native ("+out+")- failed");
    print("Can't convert arg to Native - failed");
}
catch(e) {
//    print("Can't convert arg to Native ("+e+") - passed");
    print("Can't convert arg to Native - passed");
}
*/
////////////////////
// FailInJSTest

var receiver3 = new Object();
receiver3.SetReceiver = function() {};
receiver3.FailInJSTest = function(fail) {if(fail)throw("");};
echo.SetReceiver(receiver3);

var all_ok = true;

try {
    echo.FailInJSTest(false);
}
catch(e) {
    print("FailInJSTest - failed");
    all_ok = false;
}
try {
    echo.FailInJSTest(true);
    print("FailInJSTest - failed");
    all_ok = false;
}
catch(e) {
}

if(all_ok)
    print("FailInJSTest - passed");


/***************************************************************************/

all_ok = echo.SharedString() == "a static string";
print("[shared] test - "+(all_ok ? "passed" : "failed"));

/***************************************************************************/

var e1 = new Object();
var e2 = new Object();

var v1 = new Object();
var v2 = new Object();
var v3 = new Object();
var v4 = new Object();

echo.SetReceiver(null);
all_ok = true;

v1.value = e1;
echo.SetReceiverReturnOldReceiver(v1);
all_ok = all_ok && v1.value == null;

v2.value = e2;
echo.SetReceiverReturnOldReceiver(v2);
all_ok = all_ok && v2.value == e1;

v3.value = null;
echo.SetReceiverReturnOldReceiver(v3);
all_ok = all_ok && v3.value == e2;

v4.value = e1;
echo.SetReceiverReturnOldReceiver(v4);
all_ok = all_ok && v4.value == null;

print("inout of interface tests - "+
        (all_ok ? "passed" : "failed"));
echo.SetReceiver(null);

/***************************************************************************/

try {
    echo.MethodWithForwardDeclaredParam(new Object());
    print("method with forward declared param test  -  passed");
} catch(e) {
    print("method with forward declared param test  -  failed");
}    

/***************************************************************************/

print(".......................................");
print("simple speed tests...");

var iterations = 1000;

var receiver2 = new Object();
receiver2.SetReceiver = function() {};
receiver2.SendOneString = function(str) {/*print(str);*/};

var echoJS = new Object();
echoJS.SetReceiver = function(r) {this.r = r;};
echoJS.SendOneString = function(str) {if(this.r)this.r.SendOneString(str)};
echoJS.SimpleCallNoEcho = function(){}

/*********************************************/
/*********************************************/

print("\nEcho.SimpleCallNoEcho (just makes call with no params and no callback)");
var start_time = new Date().getTime()/1000;
echoJS.SetReceiver(receiver2);
for(i = 0; i < iterations; i++)
    echoJS.SimpleCallNoEcho();
var end_time = new Date().getTime()/1000;
var interval = parseInt(100*(end_time - start_time),10)/100;
print("JS control did "+iterations+" iterations in "+interval+ " seconds.");

var start_time = new Date().getTime()/1000;
echo.SetReceiver(receiver2);
for(i = 0; i < iterations; i++)
    echo.SimpleCallNoEcho();
var end_time = new Date().getTime()/1000;
var interval = parseInt(100*(end_time - start_time),10)/100;
print("XPConnect  did "+iterations+" iterations in "+interval+ " seconds.");

/*********************************************/

print("\nEcho.SendOneString (calls a callback that does a call)");
var start_time = new Date().getTime()/1000;
echoJS.SetReceiver(receiver2);
for(i = 0; i < iterations; i++)
    echoJS.SendOneString("foo");
var end_time = new Date().getTime()/1000;
var interval = parseInt(100*(end_time - start_time),10)/100;
print("JS control did "+iterations+" iterations in "+interval+ " seconds.");

var start_time = new Date().getTime()/1000;
echo.SetReceiver(receiver2);
for(i = 0; i < iterations; i++)
    echo.SendOneString("foo");
var end_time = new Date().getTime()/1000;
var interval = parseInt(100*(end_time - start_time),10)/100;
print("XPConnect  did "+iterations+" iterations in "+interval+ " seconds.");

print(".......................................");

echoJS.SetReceiver(null);
echo.SetReceiver(null);
