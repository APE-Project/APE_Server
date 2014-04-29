/**
 * rand_chars function.
 * This function is used to create a unique
 * username of a given lenght.
 * 
 * @access public
 * @param mixed plength
 * @return void
 */
function rand_chars(plength) {
	var keylist = "abcdefghijklmnopqrstuvwxyz";
	var temp = '';
	for (i = 0; i < plength; i++) {
		temp += keylist.charAt(Math.floor(Math.random() * keylist.length));
	}
	return temp;
}


/**
 * userColor function.
 * This function is used to determine
 * the user color based on the first character
 * of his nickname.
 * 
 * @access public
 * @param mixed nickname
 * @return void
 */
function userColor(nickname) {
	var color = new Array(0, 0, 0);
	var i = 0;
	while (i < 3 && i < nickname.length) {
		color[i] = Math.abs(Math.round(((nickname.charCodeAt(i) - 97) / 26) * 200 + 10));
		i++;
	}
	return 'rgb('+color.join(', ')+')';
}