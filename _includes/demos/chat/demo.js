APE.Chat = new Class({

	Extends: APE.Client,

	Implements: Options,

	options: {
		container: null,
		logs_limit: 10,
		container: document.body
	},

	initialize: function(options) {
		this.setOptions(options);

		this.els = {};
		this.currentPipe = null;
		this.logging = true;

		this.onRaw('data', this.rawData);
		this.onCmd('send', this.cmdSend);
		this.onError('004', this.reset);

		this.onError('006', this.promptName);
		this.onError('007', this.promptName);

		this.addEvent('load', this.start);
		this.addEvent('ready', this.createChat);
		this.addEvent('uniPipeCreate', this.setPipeName);
		this.addEvent('uniPipeCreate', this.createPipe);
		this.addEvent('multiPipeCreate', this.createPipe);
		this.addEvent('userJoin', this.createUser);
		this.addEvent('userLeft', this.deleteUser);
	},

	promptName: function(errorRaw) {
		this.els.namePrompt = {};
		this.els.namePrompt.div = new Element('form', {
			'class': 'ape_name_prompt',
			'text': 'Choose a nickname : '
		}).inject(this.options.container)
		this.els.namePrompt.div.addEvent('submit', function(ev) {
			ev.stop();
			this.options.name = this.els.namePrompt.input.get('value');
			this.els.namePrompt.div.dispose();
			this.start()
		}.bindWithEvent(this));
		this.els.namePrompt.input = new Element('input', {
			'class': 'text'
		}).inject(this.els.namePrompt.div);
		new Element('input', {
			'class': 'submit',
			'type': 'submit',
			'value': 'GO!'
		}).inject(this.els.namePrompt.div)
		var error;
		if (errorRaw) {
			if (errorRaw.data.code == 007) error = 'This nick is already in use';
			if (errorRaw.data.code == 006) error = 'Bad nick, a nick must contain a-z 0-9 characters';
			if (error) {
				new Element('div', {
					'styles': {
						'padding-top': 5,
						'font-weight': 'bold'
					},
					'text': error
				}).inject(this.els.namePrompt.div);
			}
		}
	},

	start: function() {
		//If name is not set & it's not a session restore ask user for his nickname
		if (!this.options.name && !this.core.options.restore) {
			this.promptName();
		} else {
			var opt = {
				'sendStack': false,
				'request': 'stack'
			};

			this.core.start({
				'name': this.options.name
			}, opt);

			if (this.core.options.restore) {
				this.core.getSession('currentPipe', function(resp) {
					this.setCurrentPipe(resp.data.sessions.currentPipe);
				}.bind(this), opt);
			}

			this.core.request.stack.send();
		}
	},

	setPipeName: function(pipe, options) {
		if (options.name) {
			pipe.name = options.name;
			return;
		}
		if (options.from) {
			pipe.name = options.from.properties.name;
		} else {
			pipe.name = options.pipe.properties.name;
		}
	},

	getCurrentPipe: function() {
		return this.currentPipe;
	},

	setCurrentPipe: function(pubid, save) {
		save = !save;
		if (this.currentPipe) {
			this.currentPipe.els.tab.addClass('unactive');
			this.currentPipe.els.container.addClass('ape_none');
		}
		this.currentPipe = this.core.getPipe(pubid);
		this.currentPipe.els.tab.removeClass('new_message');
		this.currentPipe.els.tab.removeClass('unactive');
		this.currentPipe.els.container.removeClass('ape_none');
		this.scrollMsg(this.currentPipe);
		if (save) this.core.setSession({
			'currentPipe': this.currentPipe.getPubid()
		});
		return this.currentPipe;
	},

	cmdSend: function(data, pipe) {
		this.writeMessage(pipe, data.msg, this.core.user);
	},

	rawData: function(raw, pipe) {
		this.writeMessage(pipe, raw.data.msg, raw.data.from);
	},

	parseMessage: function(message) {
		return decodeURIComponent(message);
	},

	notify: function(pipe) {
		pipe.els.tab.addClass('new_message');
	},

	scrollMsg: function(pipe) {
		var scrollSize = pipe.els.message.getScrollSize();
		pipe.els.message.scrollTo(0, scrollSize.y);
	},

	writeMessage: function(pipe, message, from) {
		//Append message to last message
		if (pipe.lastMsg && pipe.lastMsg.from.pubid == from.pubid) {
			var cnt = pipe.lastMsg.el;
		} else { //Create new one
			//Create message container
			var msg = new Element('div', {
				'class': 'ape_message_container'
			});
			var cnt = new Element('div', {
				'class': 'msg_top'
			}).inject(msg);
			if (from) {
				new Element('div', {
					'class': 'ape_user',
					'text': from.properties.name
				}).inject(msg, 'top');
			}
			new Element('div', {
				'class': 'msg_bot'
			}).inject(msg);
			msg.inject(pipe.els.message);
		}
		new Element('div', {
			'text': this.parseMessage(message),
			'class': 'ape_message'
		}).inject(cnt);

		this.scrollMsg(pipe);

		pipe.lastMsg = {
			from: from,
			el: cnt
		};

		//notify 
		if (this.getCurrentPipe().getPubid() != pipe.getPubid()) {
			this.notify(pipe);
		}
	},

	createUser: function(user, pipe) {
		user.el = new Element('div', {
			'class': 'ape_user'
		}).inject(pipe.els.users);
		new Element('a', {
			'text': user.properties.name,
			'href': 'javascript:void(0)',
			'events': {
				'click': function(ev, user) {
					this.core.getPipe(user.pubid)
					this.setCurrentPipe(user.pubid);
				}.bindWithEvent(this, [user])
			}
		}).inject(user.el, 'inside');
	},

	deleteUser: function(user, pipe) {
		user.el.dispose();
	},

	createPipe: function(pipe, options) {
		var tmp;

		//Define some pipe variables to handle logging and pipe elements
		pipe.els = {};
		pipe.logs = new Array();

		//Container
		pipe.els.container = new Element('div', {
			'class': 'ape_pipe ape_none'
		}).inject(this.els.pipeContainer);

		//Message container
		pipe.els.message = new Element('div', {
			'class': 'ape_messages'
		}).inject(pipe.els.container, 'inside');

		//If pipe has a users list 
		if (pipe.users) {
			pipe.els.usersRight = new Element('div', {
				'class': 'users_right'
			}).inject(pipe.els.container);

			pipe.els.users = new Element('div', {
				'class': 'ape_users_list'
			}).inject(pipe.els.usersRight);;
		}
		//Add tab
		pipe.els.tab = new Element('div', {
			'class': 'ape_tab unactive'
		}).inject(this.els.tabs);

		tmp = new Element('a', {
			'text': pipe.name,
			'href': 'javascript:void(0)',
			'events': {
				'click': function(pipe) {
					this.setCurrentPipe(pipe.getPubid())
				}.bind(this, [pipe])
			}
		}).inject(pipe.els.tab);

		//Hide other pipe and show this one
		this.setCurrentPipe(pipe.getPubid());
	},

	createChat: function() {
		this.els.pipeContainer = new Element('div', {
			'id': 'ape_container'
		});
		this.els.pipeContainer.inject(this.options.container);

		this.els.more = new Element('div', {
			'id': 'more'
		}).inject(this.options.container, 'after');
		this.els.tabs = new Element('div', {
			'id': 'tabbox_container'
		}).inject(this.els.more);
		this.els.sendboxContainer = new Element('div', {
			'id': 'ape_sendbox_container'
		}).inject(this.els.more);

		this.els.sendBox = new Element('div', {
			'id': 'ape_sendbox'
		}).inject(this.els.sendboxContainer, 'bottom');
		this.els.sendboxForm = new Element('form', {
			'events': {
				'submit': function(ev) {
					ev.stop();
					var val = this.els.sendbox.get('value');
					if (val != '') {
						this.getCurrentPipe().send(val);
						this.els.sendbox.set('value', '');
					}
				}.bindWithEvent(this)
			}
		}).inject(this.els.sendBox);
		this.els.sendbox = new Element('input', {
			'type': 'text',
			'id': 'sendbox_input',
			'autocomplete': 'off'
		}).inject(this.els.sendboxForm);
		this.els.send_button = new Element('input', {
			'type': 'button',
			'id': 'sendbox_button',
			'value': ''
		}).inject(this.els.sendboxForm);
	},

	reset: function() {
		this.core.clearSession();
		if (this.els.pipeContainer) {
			this.els.pipeContainer.dispose();
			this.els.more.dispose();
		}
		this.core.initialize(this.core.options);
	}
});