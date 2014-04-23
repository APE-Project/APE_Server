/**
 * This demo uses APE to create a Vitual TCPSocket and
 * connect to an irc server (Its transparent, but it's the ape server which
 * connects to the irc server).
 *
 *
 */

var TCPSocket;
APE.IrcClient = new Class({
	Extends: APE.Client,
	Implements: Options,
	irc: null,
	connected: false,
	joined: false,
	currentChannel: null,
	version: '0.1',
	options: {
		container: document.body,
	   /*
	   irc_server: '10.1.0.30',
	   irc_port: 6667,
	   */
	   
	   irc_server: 'irc.freenode.net',
	   irc_port: 6667,
	   /*

		irc_server: 'Vancouver.BC.CA.Undernet.org',
		irc_port: 6667,
	   */
		
		original_channels:['#ape-test','#ape-project'],
		systemChannel: '@freenode',
		systemUser: '*System*',
		helpUser: '*HELP*'
	},
	els: {},
	channels: new Hash(),
	initialize: function(options){
		this.currentChannel = this.options.systemChannel;

		this.setOptions(options);
		this.joinVirtualChannel(this.options.systemChannel);

		
	},
	joinVirtualChannel: function(chan, irc, buildTabs, doNotClose){
		this.setChannel(chan, new APE.IrcClient.Channel(chan, irc, doNotClose));
		if(buildTabs){
			this.buildTabs();
		}
	},
	joinUserChannel: function(user){
		if(!this.hasChannel(user)){
			this.joinVirtualChannel(user, this.irc, true, true);
		}
	},
	complete: function(){
		
		this.core.start({name:rand_chars()});
		this.onRaw('login', this.initPlayground);
	},
	initPlayground: function(){

		
		
		this.els.chatZone = $('history');
		this.els.tabs = $('tabs');
		this.els.input = $('chat-input');
		this.els.userList = $('user-list');
		this.els.userCnt = $('usr_cnt');

		//Adding special events
		this.els.input.addEvent('keydown', this.sendKey.bindWithEvent(this));


		//TCPSocket implementation
		TCPSocket = this.core.TCPSocket;

		//IRC events
		this.irc = new IRCClient();
		this.irc.onopen  = this.onopen.bind(this);
		this.irc.onclose = this.onclose.bind(this);
		this.irc.onTOPIC = this.onTOPIC.bind(this);
		this.irc.onNICK = this.onNICK.bind(this);
		this.irc.onJOIN = this.onJOIN.bind(this);
		this.irc.onQUIT = this.onQUIT.bind(this);
		this.irc.onPART = this.onPART.bind(this);
		this.irc.onACTION = this.onACTION.bind(this);
		this.irc.onCTCP  = this.onCTCP.bind(this);
		this.irc.onNOTICE  = this.onNOTICE.bind(this);
		this.irc.onPRIVMSG = this.onPRIVMSG.bind(this);
		this.irc.onMODE = this.onMODE.bind(this);
		this.irc.onERROR = this.onerror.bind(this);
		this.irc.onerror = this.onerror.bind(this);
		this.irc.onresponse = this.onresponse.bind(this);


	},
	changeMyNick: function(nick){
		this.nickname = nick;
		
		$$('.show-nick').set('html', nick);
	},
	setNick: function(nickname){
		if(!this.irc) return false;
		nickname = this.cleanNick(nickname);
		this.irc.connect(this.options.irc_server, this.options.irc_port);
		this.changeMyNick(nickname);
		return true;
	},
	/** CHANNELS ACCESSORS */
	getChannel: function(key){
		return this.channels.get(this.toChan(key));
	},
	hasChannel: function(key){
		return this.channels.has(this.toChan(key));
	},
	setChannel: function(key, chan){
		this.channels.set(this.toChan(key), chan);
	},
	eraseChannel: function(key){
		this.channels.erase(this.toChan(key));
	},
	compareChannels: function(a, b){
		return this.toChan(a) == this.toChan(b);
	},
	/**
	 * connect() is called two times, but only does something when irc is connected
	 * AND nickname is set
	 */
	connect: function(){
		if(this.nickname && this.connected){
			this.irc.ident(this.nickname, '8 *', this.nickname);
			this.irc.nick(this.nickname);
		}
	},
	onopen: function(){
		
		this.connected = true;
		this.connect();
		/*
		window.addEvent('unload', function(){
			this.irc.reset();
		}.bind(this));
		*/
	},
	onclose: function(){
		
	},
	onerror: function(cmd){
		if(cmd.args[0] == "Closing Link: 127.0.0.1 (Connection Timed Out)"){
			this.showInfo('Error occured, reconnecting...', 'error');
			this.irc.connect(this.options.irc_server, this.options.irc_port);
			return;
		}
		var responseCode = parseInt(cmd.type);
		if (responseCode == 431 || responseCode == 432 || responseCode == 433) {
		// 431	 ERR_NONICKNAMEGIVEN
		// 432	 ERR_ERRONEUSNICKNAME
		// 433	 ERR_NICKNAMEINUSE
			var nick = cmd.args[1];

			if(responseCode == 432){
				nick = this.cleanNick(nick);
				this.changeMyNick(nick);
			}else
				if(nick.length == 15){

				}

				nick = (nick.length>= 15?nick.substr(1, 14):nick) + '_';
				this.changeMyNick(nick);
				this.irc.nick(nick);
				//this.irc.ident(this.nickname, '8 *', this.nickname);
		}else if(responseCode == 451){
			this.irc.ident(this.nickname, '8 *', this.nickname);
		}else{
			this.showInfo(this.sanitize(cmd.args.pop()), 'error');
		}
	},
	onresponse: function(cmd){
		var responseCode = parseInt(cmd.type);
		if(responseCode==372){
			this.addMessage(this.options.systemChannel, this.options.systemUser, cmd.args[1].substr(2), 'info');
			
			if(!this.joined){
				var cnt = this.options.original_channels.length;

				for(var i = 0; i < cnt; i ++) {
					this.joinChannel(this.options.original_channels[i], i==0);
				}
				
			}
		}else if(responseCode==366){
			this.addMessage(this.options.systemChannel, this.options.systemUser, 'Joined '+cmd.args[1]+' channel.', 'info user-join');
		}
		else if(responseCode==353){
			var channel = cmd.args[2];
			var userList = cmd.args[3].split(/\s+/);
			
			userList.each(function(user){
				if(user != '')
					this.addUser(channel, user, false, false);
			}.bind(this));
			if(this.compareChannels(channel,this.currentChannel)){
				var chan = this.getCurrentChannel();
				chan.sortUsers();
				this.buildUsers();
			}

		}
		else if(responseCode==332){
			this.addMessage(cmd.args[1], this.options.systemUser, ['Topic for '+cmd.args[1]+' is: ',new Element('i',{'text':this.sanitize(cmd.args[2])})],'info topic');
		}
		else if(responseCode==333){
			var d = new Date();
			var time = parseInt(cmd.args[3], 10);
			d.setTime(time*1000);
			var when = d.toString();
			this.addMessage(cmd.args[1], this.options.systemUser, ['Topic for '+cmd.args[1]+' was set by ',this.userLink(this.parseName(cmd.args[2])),' on '+when], 'info topic');
		}
	},
	/**
	 * NAMES
	 * CTCP VERSION
	 */
	onJOIN: function(cmd){
		var chan = cmd.args[0];
		var user = this.parseName(cmd.prefix);
		this.addUser(chan, user);
	},
	onMODE: function(cmd){
		
		var chan = this.getChannel(cmd.args[0]);
		if(chan){
			var from = this.parseName(cmd.prefix);
			var user = cmd.args[2];
			var op = cmd.args[1];
			if(op.substr(0,1)=='+'){
				var build = false;
				if(op.contains('o')){
					this.addMessage(chan, this.options.systemUser,
					[this.userLink(from),' gives channel operator status to ',this.userLink(user)], 'info status');
					chan.renameUser(user, '@'+user);
					build = true;
				}else if(op.contains('v')){
					this.addMessage(chan, this.options.systemUser,
					[this.userLink(from),' gives channel operator status to ',this.userLink(user)], 'info status');
					chan.renameUser(user, '+'+user);
					build = true;
				}
				if(build){
					this.buildUsers();
				}
			}
		}
	},
	onPART: function(cmd){
		var chan = this.getChannel(cmd.args[0]);
		if(chan){
			var user = this.parseName(cmd.prefix);
			var msg = String(cmd.args[1]);

			chan.remUser(user);
			this.addMessage(chan.name, this.options.systemUser, user+' has left '+chan.name+(msg.length>0?' ('+msg+')':''), 'user-left');
			if(chan.name==this.currentChannel){
				this.remUser(user);
			}
		}
	},
	onQUIT: function(cmd){
		var user = this.parseName(cmd.prefix);
		this.channels.each(function(chan){
			if(chan.remUser(user)){
				this.addMessage(chan.name, this.options.systemUser, [this.userLink(user),' has quit ('+this.sanitize(cmd.args[0])+')'], 'info user-left');
				if(chan.name==this.currentChannel){
					this.remUser(user);
				}
			}
		}.bind(this));
	},
	onNICK: function(cmd){
		this.changeNick(this.parseName(cmd.prefix), cmd.args[0]);
	},
	onTOPIC: function(cmd){
		var user = this.parseName(cmd.prefix);
		this.addMessage(cmd.args[0], this.options.systemUser, user+' has changed the topic to: '+cmd.args[1], 'info topic');
	},
	onCTCP: function(cmd){
		if(cmd.args[1]=='VERSION'){
			this.irc.ctcp(this.parseName(cmd.prefix), 'VERSION APE TCPSocket Demo (IRC) 2 - http://www.ape-project.com v'+this.version+' On a Web Browser (' + navigator.appCodeName + ')', true);
		}else{
			
		}
	},
	onACTION: function(cmd){
		var user = this.parseName(cmd.prefix);
		var args = cmd.args;
		this.addMessage(args.shift(), this.options.systemUser, [this.userLink(user),' '+this.sanitize(args.join(' '))], 'info action');
	},
	onNOTICE: function(cmd){
		var msg = cmd.args[1];
		if(msg.charCodeAt(0)==1 && msg.charCodeAt(msg.length - 1) == 1){
			// CTCP REPLY
			msg = msg.substr(1,msg.length-2).split(' ');
			this.addCTCP(cmd.args[0], msg.shift(), msg.join(' '));
		}else{
			if(cmd.prefix && cmd.prefix.substr(0,3)=='irc')
				this.addMessage(this.options.systemChannel, this.options.systemUser, this.sanitize(cmd.args[1]), 'info notice');
			else
				this.showInfo(this.sanitize(cmd.args[1]), 'notice');
		}
	},
	onPRIVMSG: function(cmd){
		var from = this.parseName(cmd.prefix);
		var chan = cmd.args[0];
		var msg = cmd.args[1];

		if(this.compareChannels(chan, this.nickname)){
			this.joinUserChannel(from);
			this.addUser(from, from);
			chan = from;
		}
		this.addMessage(chan, from, this.sanitize(msg));
	},
	addCTCP: function(who, what, txt){
		if(txt != undefined){
			this.addMessage(this.currentChannel, '-'+who+'-', 'CTCP '+what+' REPLY: '+txt, 'ctcp');
		}else{
			this.addMessage(this.currentChannel, '>'+who+'<', 'CTCP '+what, 'ctcp')
		}
	},
	getCurrentChannel: function(){
		return this.getChannel(this.currentChannel);
	},
	joinChannel: function(channel, switchTo){
		
		if(!this.hasChannel(channel)){
			var chan = new APE.IrcClient.Channel(channel, this.irc);
			this.setChannel(channel, chan);
			chan.join();
			if(switchTo!==false) this.switchTo(channel);
			else this.buildTabs();
		}else if(switchTo!==false){
			this.switchTo(channel);
		}
		this.joined = true;
	},
	switchTo: function(chan){

		if(!this.hasChannel(chan)){
			return;
		}
		var channel = this.getChannel(chan);

		/** For history */
		channel.Hcnt = 0;

		this.currentChannel = chan;

		/* Updating messages and users*/
	   this.els.chatZone.getElements('div.msg_line').dispose();


		this.buildTabs();
		
		this.buildUsers();

		this.checkLine();
		
		this.buildMsg();
	},
	cleanNick: function(nick){
		var ret = String(nick).replace(/[^a-zA-Z0-9\-_~]/g, '');
		ret = ret.replace(/^[0-9]+/, '');
		if(ret=='') return 'Guest'+Math.round(Math.random()*100);
		
		if(nick != ret) this.showError('Invalid nick "'+this.sanitize(nick)+'" changed to "'+ret+'".');
		return ret;
	},
	changeNick: function(from, to){
		this.channels.each(function(chan){
			if(chan.renameUser(from, to)){
				this.addMessage(chan.name, this.options.systemUser, [this.userLink(from)," is now known as ", this.userLink(to)], 'info');
			}
			if(this.compareChannels(chan.name, from)){
				this.changeChannelName(from, to);
			}
		}.bind(this));
		if(this.compareChannels(from, this.nickname))
			this.changeMyNick(to);
		this.buildUsers();
	},
	changeChannelName: function(from, to){

		var chan = this.getChannel(from);
		this.eraseChannel(from);
		chan.name = to;
		this.setChannel(to, chan);
		if(this.compareChannels(from, this.currentChannel)){
			this.currentChannel = from;
		}
		this.buildTabs();
	},
	parseName: function(identity){
		return identity.split("!", 1)[0];
	},
	remUser: function(user){
		var go = new Fx.Morph($('user_line_'+user));
		go.start({
			'height': 0
		});
		go.addEvent('complete', function(el){
			el.destroy();
		})

		this.userCntAdd(-1);
	},
	addUser: function(chan, user, showMessage, addNow){

		var channel =  this.getChannel(chan);
		if(!channel){
			return;
		}
		var add;
		if(add = channel.addUser(user, addNow)){
			if(this.compareChannels(chan, this.currentChannel) && addNow !== false){

				if(channel.getLastUser().type != 'zzz' || add == 'rename'){
					this.buildUsers();
				}else{
					this.writeUser(channel.buildLastUser( this.userClick.bindWithEvent(this) ));
					this.userCntAdd(1);
				}
			}
			if(showMessage !== false)
				this.addMessage(chan, this.options.systemUser, [this.userLink(user),' has joined'+(chan==user?'':' '+chan)], 'info user-join');
		}
	},
	writeUser: function(el_user){
		this.els.userList.adopt(el_user);
	},
	addMessage: function(chan, user, txt, special){
		var channel = this.getChannel(chan);
		if(channel){
			if(user != this.options.systemUser) user = this.userLink(user);
			channel.addMessage(user, txt, special, this.nickname);
			if(this.compareChannels(chan, this.currentChannel)){
				this.writeMsg(channel.buildLastMessage());
			}
		}
	},
	writeMsg: function(el_line){
		this.els.chatZone.adopt(el_line);
		this.scrollBottom();
		this.checkLine();
	},
	checkLine: function(){
		var chan = this.getCurrentChannel();
		var lineW = chan.getLineWidth();
		var textW = 590 - lineW;

		var line_size = Math.round((textW - 10) / 6);
		chan.line_size = line_size;

		// TODO Resize long lines

		$$('.msg_user').tween('width', lineW);
		$('line').tween('left', lineW);
		$$('.msg_text').tween('width', textW);
	},
	scrollBottom: function(){
		var scrollSize = this.els.chatZone.getScrollSize();

		this.els.chatZone.scrollTo(0,scrollSize.y);
	},
	sendMsg: function(msg, checkCmd){

		var channel = this.getCurrentChannel();

		channel.history.unshift(msg);
		channel.Hcnt = 0;

		//This is a command
		if(checkCmd !== false && msg.substr(0,1)=='/')
			this.sendCmd(msg.substr(1).split(' '));
		else{
			channel.send(msg);
			// the IRC server will not echo our message back, so simulate a send.
			this.onPRIVMSG({prefix:this.nickname,type:'PRIVMSG',args:[this.currentChannel, msg]});
		}
	},
	sendCmd: function(args){
		switch(String(args[0]).toLowerCase()){
			case 'j':
			case 'join':
				if(!args[1] || !args[1].match(/^(#|&)[^ ,]+$/))
					this.showError('Invalid chan name "'+this.sanitize(args[1])+'" chan name must begin with # or & and must not contains any of \' \' (space) or \',\' (comma) and must contains at least 2 chars.')
				else
					this.joinChannel(args[1])
				break;
			case 'clear':
				if(!args[1] || args[1]!='ALL'){
					this.getCurrentChannel().clear();
				}else{
				   this.channels.each(function(chan){
					   chan.clear();
				   });
				}
				this.els.chatZone.empty();
				break;
			case 'quit':
				args.shift();
				window.location.reload();
				this.irc.quit(args.join(' '));
				break;
			case 'ctcp':
				args.shift();
				var user = args.shift();

				this.irc.ctcp(user, args.join(' '));
				this.addCTCP(user, args[0]);
				break;
			case 'me':
			case 'action':
				if(!args[1]){
					this.showError('Invalid arguments, /help for more informations');
				}
				args.shift();
				this.irc.action(this.currentChannel, args.join(' '));
				this.onACTION({prefix:this.nickname,args:[this.currentChannel, args.join(' ')]});
				break;
			case 'msg':
			case 'privmsg':
				if(args[1] == undefined || args[2] == undefined ){
					this.showError('Invalid arguments, /help for more informations');
					break;
				}
				args.shift();

				var to = String(args.shift());
				if(to.charAt(0)=='@') to = to.substr(1);
				
				var msg = args.join(' ');

				this.joinUserChannel(to);
				
				this.switchTo(to);
				this.sendMsg(msg, false);
				
				break;
			case 'nick':
				this.irc.nick(args[1]);
				//this.changeMyNick(args[1]);
				break;
			case 'h':
			case 'help':
				this.addMessage(this.currentChannel, this.options.helpUser,'\
List of available commands :\n\
\n\
	/HELP , show this help.\n\
	/H , alias for /HELP.\n\
	/JOIN <channel>, joins the channel.\n\
	/CLEAR [ALL], clear current channels, clears messages and input history.\n\
	/QUIT [<reason>], disconnects from the server.\n\
	/CTCP <nick> <message>, send a CTCP message to nick, VERSION and USERINFO are commonly used.\n\
	/ACTION <action>, send a CTCP ACTION message, describing what you are doing.\n\
	/PRIVMSG [@]<nick> <message>, sends a private message.\n\
	/MSG [@]<nick> <message>, alias for /PRIVMSG.\n\
	/NICK <nick>, sets you nickname.\n\
\n\
Commands are case insensitive, for example you can user /join or /JOIN.','help')
				break;
			default:
				this.showError('Unknow or unsuported command "'+this.sanitize(args[0])+'".');
		}
	},
	showInfo: function(msg, type){
		this.addMessage(this.currentChannel, this.options.systemUser, msg, type);
	},
	showError: function(msg){
		this.showInfo(msg, 'error')
	},
	sendKey: function(ev){
		if(ev.key == 'enter') this.sendClick();
		else if(ev.key == 'up'){
			var chan = this.getCurrentChannel();
			if(chan.Hcnt == 0){
				chan.currMsg = this.els.input.value;
			}
			if(chan.history.length > chan.Hcnt){
				var i = chan.Hcnt++;
				this.els.input.value = chan.history[i];
			}
		}else if(ev.key == 'down'){
			var chan = this.getCurrentChannel();
			if(chan.Hcnt > 0){
				var i = --chan.Hcnt;

				this.els.input.value = i==0?chan.currMsg:chan.history[i-1];
			}
		}else if(ev.key=='tab'){
			ev.stop();
			var val = this.els.input.value;
			if(val.charAt(0)=='/' && !val.contains(' ')){
				val = val.substr(1);
				var cmd_list = new Array(
					'help',
					'join',
					'clear',
					'quit',
					'ctcp',
					'action',
					'privmsg',
					'nick',
					'msg'
				);
				var check = function(cmd, index,array){
					if(cmd.substr(0, val.length)==val){
						return true;
					}
					return false;
				}
				var ok = cmd_list.filter(check);
				if(ok.length > 0){
					this.els.input.value = '/'+ok[0].toUpperCase()+' ';
				}
			}else{
			val = val.split(' ');
			var mot = val.pop();
			if(mot.charAt(0)=='@') mot = mot.substr(1);
				if(mot.length > 0){
					var chan = this.getCurrentChannel();
					var usr = chan.search(mot);
					if(usr){
						this.els.input.value = val.join(' ')+(val.length > 0?' ':'')+'@'+usr+' ';
					}
				}
			}
		}
	},
	sendClick: function(){

		var value = this.els.input.value;
		if(value.length > 0){
			if(value.substr(0, 1)=='/' || this.currentChannel != this.options.systemChannel){
				this.sendMsg(this.els.input.value);
			}else{
				this.showError('No channel joined. Try /join #<channel>');
			}
		}
		this.els.input.value = '';
	},
	tabClick: function(event, chan){
		this.switchTo(chan);
	},
	tabCloseClick: function(event, chan){
		this.closeTab(chan);
		event.stop();
	},
	buildTabs: function(){
		/*
		<div class="tab">
			<sa class="close"></a>
			<span class="link">
				#ape-project
			</span>
		</div>
		 */
		var current = this.currentChannel;
		this.els.tabs.empty();
		var tabs = new Array();
		this.channels.each(function(chan, key){
			var el_tab = new Element('div', {
				'class': 'tab'+(this.compareChannels(chan.name, current)?' current':'')+(this.compareChannels(key, this.options.systemChannel)?' sys':''),
				'id': 'tab_'+key
			});
			var el_link = new Element('span', {
				'text': chan.name,
				'class': 'link'
			});
			var el_close = new Element('a', {
				'href': '#'
			});
			el_tab.grab(el_close);
			el_tab.grab(el_link);

			if(!this.compareChannels(chan.name, current))
				el_link.addEvent('click', this.tabClick.bindWithEvent(this, key));
			
			if(!this.compareChannels(chan.name, this.options.sysChannel))
				el_close.addEvent('click', this.tabCloseClick.bindWithEvent(this, key));

			tabs.push(el_tab);
		}.bind(this));
		this.els.tabs.adopt(tabs);
		this.els.input.focus();
	},
	closeTab: function(tab){
		
		if(this.compareChannels(tab,this.currentChannel)){
			var keys = this.channels.getKeys();
			var i = keys.indexOf(tab);
			var k = 0;
			if(i==keys.length-1){
				k = i - 1;
			}else{
				k = i+1;
			}
			this.switchTo(keys[k]);
		}
		var chan = this.getChannel(tab);
		chan.close('');
		this.eraseChannel(tab);
		$('tab_'+tab).destroy();
	},
	buildUsers: function(){
		this.els.userList.empty();

		var users = this.getCurrentChannel().buildUsers(this.userClick.bindWithEvent(this));
		this.writeUser(users);
		this.userCntSet(users.length);
	},
	userCntSet: function(cnt){
		this.els.userCnt.set('text', cnt);
		this.els.userCnt.store('cnt', cnt);
	},
	userCntAdd: function(num){
		var cnt = this.els.userCnt.retrieve('cnt', 0);
		this.userCntSet(cnt+num);
	},
	userClick: function(ev, e2){
		ev.stop();
		var to = String(e2).replace(/^(@|\+)/g, '');
		if(!this.hasChannel(to)){
			this.joinVirtualChannel(to, this.irc, true, true);
		}
		this.switchTo(to);
	},
	buildMsg: function(){
		this.writeMsg(this.getCurrentChannel().buildMessages());
	},
	toChan: function(str){
		return String(str).toLowerCase();
	},
	userLink: function(user){
		var link = new Element('a', {
			'text': user,
			'class': 'user',
			'href': '#'
		});
		link.addEvent('click', this.userClick.bindWithEvent(this, user));
		return link;
	},
	sanitize: function(str){
		//all text elements are grabed via appendText, so there is no need to escape
		return str;
	}
	/*
	sanitize: (function(str) {
	  // See http://bigdingus.com/2007/12/29/html-escaping-in-javascript/
	  var MAP = {
		'&': '&amp;',
		'<': '&lt;',
		'>': '&gt;',
		'"': '&quot;',
		"'": '&#39;'
	  };
	  var repl = function(c) { return MAP[c]; };
	  return function(s) {
		s = s.replace(/[&<>'"]/g, repl);
		return s;
	  };
	})()
	*/
});
