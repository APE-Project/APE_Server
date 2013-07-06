//http://point.davidglasser.net/2013/06/27/surprising-javascript-memory-leak.html
//So in summary: If you have a large object that is used by some closures, but not by any closures that you need to keep using, just make sure that the local variable no longer points to it once you’re done with it.

//yep: Ape needs also humans to think for themselves :-(
var run = function() {
	var str = new Array(1000000).join('*');
	var doSomethingWithStr = function() {
	if (str === 'something')
		Ape.log('str was something');
	};
	doSomethingWithStr();
	var logIt = function() {
		Ape.log('interval');
	};
	Ape.setInterval(logIt, 100);
};
Ape.setInterval(run, 1000);

