var userlist = new $H;

Ape.registerHookCmd("connect", function(params, infos) {
	if (!$defined(params.name) || userlist.has(params.name.toLowerCase()) || params.name.length > 16 || params.name.test('[^a-z]', 'i')) {
		return 0;
	}
	userlist.set(params.name.toLowerCase(), true);
	return {"name": params.name};
});

Ape.addEvent('deluser', function(user) {
	userlist.erase(user.getProperty('name').toLowerCase());
});
