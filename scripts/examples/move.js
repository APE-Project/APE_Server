Ape.registerCmd('setpos', true, function(params, infos) {
	if ((!$defined(params.x) || !$defined(params.y)) && (!isFinite(params.x) || !isFinite(params.y))) return 0;
	
	//Set the user properties for the new coordinates
	infos.user.setProperty('x', params.x);
	infos.user.setProperty('y', params.y);
	
	//Get the channel where we need to send the callback
	var chan = Ape.getChannelByPubid(params.pipe);
	
	//Test if channel exist
	if (chan) {
		
		//Send a raw to the channel pipe.
		chan.pipe.sendRaw('positions', {'x': params.x, 'y': params.y}, {'from': infos.user.pipe});
		
		//The raw won't be sent to our user since the 'from' parameter is used. But we still want our 
		//user to get the raw (and we need 'from'). So we send also only to our user who sent the command
		infos.user.pipe.sendRaw('positions', {'x': params.x, 'y': params.y}, {'from': infos.user.pipe});
		
	} else {
		return ['109', 'UNKNOWN_PIPE'];
	}
	
	return 1;
});