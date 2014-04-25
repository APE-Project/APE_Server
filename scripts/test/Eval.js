Ape.log(' =====================================');
Ape.log('       Start up for test/Eval.js     ');
Ape.log(' =====================================\n');

Ape.log(' >>> Test #1:');
var code = '4 + 4;';
var ret = Ape.eval(code);
if (ret == 8) {
	Ape.log('OK\n');
} else {
	Ape.log('Failed\n');
}

Ape.log(' >>> Test #2:');
var code = 'sum = function(a, b){return a + b;}; sum(4,4);';
var ret = Ape.eval(code);
if (ret == 8) {
	Ape.log('OK\n');
} else {
	Ape.log('Failed: ' + ret + '\n');
}

Ape.log(' >>> Test #3:');
var code = 'Ape.log(\'Ape is nice !\'); null';
var ret = Ape.eval(code);
if (ret === null) {
	Ape.log('OK\n');
} else {
	Ape.log('Failed: ' + ret + '\n');
}
Ape.log(' >>> Test #4:');
eval("Ape.log('OK');");

Ape.log("\n\n");