var userlist = new $H;

Ape.registerHookCmd("connect", function(params, infos) {
	if (!$defined(params.name)) return 0;
	if (userlist.has(params.name.toLowerCase())) return ["005", "NICK_USED"];
	if (params.name.length > 16 || params.name.test('[^a-z]', 'i')) return ["006", "BAD_NICK"];
	
	userlist.set(params.name.toLowerCase(), true);

	return {
		'properties': {
			'name':params.name
		}
	};
});

Ape.addEvent('deluser', function(user) {
	userlist.erase(user.getProperty('name').toLowerCase());
});
