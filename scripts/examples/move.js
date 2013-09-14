Ape.registerCmd('setpos', true, function(params, infos) {
	if ((!$defined(params.x) || !$defined(params.y)) && (!isFinite(params.x) || !isFinite(params.y))) return 0;

	infos.user.setProperty('x', params.x);	
	infos.user.setProperty('y', params.y);	
	
	var chan = Ape.getChannelByPubid(params.pipe); 

	if (chan) {
		chan.pipe.sendRaw('positions', {'x': params.x, 'y': params.y}, {'from': infos.user.pipe}); 
	} else {
		return ['109', 'UNKNOWN_PIPE'];
	}

	return 1;
});
