var userlist = new $H;

Ape.registerHookCmd("connect", function(params, infos) {
	if (!$defined(params.nickname) || userlist.has(params.nickname.toLowerCase()) || params.nickname.length > 16 || params.nickname.test('[^a-z]', 'i')) {
		return 0;
	}
	userlist.set(params.nickname.toLowerCase(), true);
	return {"name": params.nickname};
});

Ape.addEvent('deluser', function(user) {
	userlist.erase(user.getProperty('name').toLowerCase());
});
