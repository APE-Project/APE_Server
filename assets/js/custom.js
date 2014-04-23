//Fix for dropdown on iPad : https://github.com/twbs/bootstrap/issues/2975#issuecomment-9839064
$('body')
	.on('touchstart.dropdown', '.dropdown-menu', function (e) { e.stopPropagation(); })
	.on('touchstart.dropdown', '.dropdown-submenu', function (e) { e.preventDefault(); });