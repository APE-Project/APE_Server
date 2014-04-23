(function() {
	var password = Ape.config("inlinepush.conf", "password");
	Ape.registerCmd("inlinepush", false, function(params, infos) {
		if (params.password == password) {
		
			if ($defined(params.channel) && $defined(params.data) && $defined(params.raw)) {
				var chan = Ape.getChannelByName(params.channel);
				if (!$defined(chan)) return ["401", "UNKNOWN_CHANNEL"];
			
				chan.pipe.sendRaw(params.raw, params.data);
			
				return {"name":"pushed","data":{"value":"ok"}};
			} else {
				return 0;
			}
		} else {
			return ["400", "BAD_PASSWORD"];
		}

	})
})()