Ape.log(' =====================================>>> \n Start up for test/Properties.js\n');

Ape.log(' 1 =====================================>>> \n\n');
Ape.registerCmd('testNullProperty', true, function(params, info) {
	myChan = Ape.mkChan('testChan');
	myChannel.setProperty('testKeyChan', null);
	info.user.setProperty('testKeyUser', null);
	info.user.pipe.setProperty('tesKeyPipe', null);
	return 0;
});

Ape.log('<<< =====================================\n');
