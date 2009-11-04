var userlist = new $H;

Ape.registerHookCmd("connect", function(params, cmd) {

	if (!$defined(params.name)) return 0;
	if (userlist.has(params.name.toLowerCase())) return ["005", "NICK_USED"];
	if (params.name.length > 16 || params.name.test('[^a-zA-Z0-9]', 'i')) return ["006", "BAD_NICK"];
	
	userlist.set(params.name.toLowerCase(), true);
	
	cmd.user.setProperty('name', params.name);
	
	return 1;
});

Ape.addEvent('deluser', function(user) {
	userlist.erase(user.getProperty('name').toLowerCase());
});
