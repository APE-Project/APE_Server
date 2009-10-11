Ape.registerCmd("control", false, function(params, infos) {
	if (params.password == Ape.config("control.conf", "password")) {
		var chan = Ape.getChannelByName(params.channel).pipe.sendRaw(params.raw, params.data);
	}
})