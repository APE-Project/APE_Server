Ape.log(' =====================================');
Ape.log('    Start up for test/Properties.js   ');
Ape.log(' =====================================\n');

Ape.log(' >>> Testing null properties');
Ape.registerCmd('testNullProperty', true, function(params, info) {
	myChan = Ape.mkChan('testChan');
	myChannel.setProperty('testKeyChan', null);
	info.user.setProperty('testKeyUser', null);
	info.user.pipe.setProperty('tesKeyPipe', null);
	return 0;
});

Ape.log("\n\n");