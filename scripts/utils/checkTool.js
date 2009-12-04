Ape.registerCmd('setup', false, function(params, infos) {
	var domain = Ape.config('ape.conf', 'domain');
	if (domain == 'auto') domain = params.domain;
	return {"name": "setupResponse", "data": {"domain": domain}};
});
