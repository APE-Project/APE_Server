Ape.log(' =====================================>>> \n Start up for test/Eval.js\n');

Ape.log(' 1 =====================================>>> \n\n');
var code = '4 + 4;';
var ret = Ape.eval(code);
if (ret == 8) {
	Ape.log('OK\n');
} else {
	Ape.log('Failed\n');
}

Ape.log(' 2 =====================================>>> \n\n');
var code = 'sum = function(a, b){return a + b;}; sum(4,4);';
var ret = Ape.eval(code);
if (ret == 8) {
	Ape.log('OK\n');
} else {
	Ape.log('Failed: ' + ret + '\n');
}
Ape.log(' 3 =====================================>>> \n\n');
var code = 'Ape.log(\'This is weird.\'); null';
var ret = Ape.eval(code);
if (ret === null) {
	Ape.log('OK\n');
} else {
	Ape.log('Failed: ' + ret + '\n');
}
Ape.log(' 4 =====================================>>> \n\n');
eval("Ape.log('OK');");
Ape.log('<<< =====================================\n');
