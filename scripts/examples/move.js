Ape.registerCmd('setpos', true, function(params, info) {
	if ((!$defined(params.x) || !$defined(params.y)) && (!isFinite(params.x) || !isFinite(params.y))) return 0;
	info.user.setProperty('x', params.x);
	info.user.setProperty('y', params.y);
	var chan = Ape.getChannelByPubid(params.pipe);
	if (chan) {
		chan.pipe.sendRaw('positions', {'x': params.x, 'y': params.y}, {'from': info.user.pipe});
	} else {
		return ['109', 'UNKNOWN_PIPE'];
	}
	return 1;
});
