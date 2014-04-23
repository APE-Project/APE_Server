APE.Move = new Class({
	Extends: APE.Client,
	Implements: Options,
	options: {
		container: document.body
	},
	initialize: function(options) {
		this.setOptions(options);
		this.addEvent('ready', this.initPlayground);
		this.addEvent('userJoin', this.createUser);
		this.addEvent('multiPipeCreate', function(pipe, options) {
			this.pipe = pipe;
		});
		this.addEvent('userLeft', this.deleteUser);
		this.onRaw('positions', this.rawPositions);
		this.onRaw('data', this.rawData);
		this.onCmd('send', this.cmdSend);
		this.onError('004', this.reset);
	},
	deleteUser: function(user, pipe) {
		user.element.dispose();
	},
	cmdSend: function(param, pipe) {
		this.writeMessage(pipe, param.msg, this.core.user);
	},
	rawData: function(raw, pipe) {
		this.writeMessage(pipe, raw.data.msg, raw.data.from);
	},
	rawPositions: function(raw, pipe) {
		this.movePoint(raw.data.from, raw.data.x, raw.data.y);
	},
	parseMessage: function(message) {
		return decodeURIComponent(message);
	},
	writeMessage: function(pipe, message, sender) {
		//Define sender
		sender = this.pipe.getUser(sender.pubid);
		//destroy old message
		sender.element.getElements('.ape_message_container').destroy();
		//Create message container
		var msg = new Element('div', {
			'opacity': '0',
			'class': 'ape_message_container'
		});
		var cnt = new Element('div', {
			'class': 'msg_top'
		}).inject(msg);
		//Add message
		new Element('div', {
			'text': this.parseMessage(message),
			'class': 'ape_message'
		}).inject(cnt);
		new Element('div', {
			'class': 'msg_bot'
		}).inject(cnt);
		//Inject message
		msg.inject(sender.element);
		//Show message
		var fx = msg.morph({
			'opacity': '1'
		});
		//Delete old message
		(function(el) {
			$(el).morph({
				'opacity': '0'
			});
			(function() {
				$(this).dispose();
			}).delay(300, el);
		}).delay(3000, this, msg);
	},
	createUser: function(user, pipe) {
		if (user.properties.x) {
			var x = user.properties.x;
			var y = user.properties.y;
		} else {
			var x = 8;
			var y = 8;
		}
		var pos = this.element.getCoordinates();
		x = x.toInt() + pos.left;
		y = y.toInt() + pos.top;
		user.element = new Element('div', {
			'class': 'demo_box_container',
			'styles': {
				'left': x + 'px',
				'top': y + 'px'
			}
		}).inject(this.element, 'inside');
		new Element('div', {
			'class': 'user',
			'styles': {
				'background-color': 'rgb(' + this.userColor(user.properties.name) + ')'
			}
		}).inject(user.element, 'inside');
		var span = new Element('span', {
			'text': user.properties.name
		}).inject(user.element, 'inside');
		if (user.pubid == this.core.user.pubid) {
			span.addClass('you');
			span.set('text', 'You');
			if (!this.core.options.restore) {
				var offset = this.element.getPosition();
				this.sendpos($random(0, 640) + offset.x, $random(0, 300) + offset.y);
			}
		}
	},
	userColor: function(nickname) {
		var color = new Array(0, 0, 0);
		var i = 0;
		while (i < 3 && i < nickname.length) {
			color[i] = Math.abs(Math.round(((nickname.charCodeAt(i) - 97) / 26) * 200 + 10));
			i++;
		}
		return color.join(', ');
	},
	sendpos: function(x, y) {
		var pos = this.posToRelative(x, y);
		this.pipe.request.send('SETPOS', {
			'x': pos.x.toInt(),
			'y': pos.y.toInt()
		});
		this.movePoint(this.core.user, pos.x, pos.y);
	},

	posToRelative: function(x, y) {
		var pos = this.element.getCoordinates();
		x = x - pos.left - 36;
		y = y - pos.top - 46;
		if (x < 0) x = 10;
		if (x > pos.width) x = pos.width - 10;
		if (y < 0) y = 10;
		if (y > pos.height) y = pos.height - 10;
		return {
			'x': x,
			'y': y
		};
	},
	movePoint: function(user, x, y) {
		var user = this.pipe.getUser(user.pubid);
		var el = user.element;
		var fx = el.retrieve('fx');
		if (!fx) {
			fx = new Fx.Morph(el, {
				'duration': 300,
				'fps': 100
			});
			el.store('fx', fx);
		}
		el.retrieve('fx').cancel();
		pos = this.element.getCoordinates();
		x = x.toInt();
		y = y.toInt();
		//Save position in user properties
		user.properties.x = x;
		user.properties.y = y;
		fx.start({
			'left': pos.left + x,
			'top': pos.top + y
		});
	},
	initPlayground: function() {
		this.element = this.options.container;
		this.els = {};
		this.els.move_box = new Element('div', {
			'class': 'move_box'
		}).inject(this.element);
		this.els.move_box.addEvent('mousedown', function(ev) {
			ev.stop();
			this.sendpos(ev.page.x, ev.page.y);
		}.bindWithEvent(this));
		var el1 = new Element('div', {
			'id': 'moveOverlay',
			'styles': {
				'opacity': 0.5
			}
		}).inject(this.element, 'top');
		var el2 = new Element('div', {
			'id': 'moveOverlay',
			'styles': {
				'background': 'none',
				'z-index': 6
			},
			'text': 'Click on the grey area to move your ball'
		}).inject(this.element, 'top');
		var clear = function() {
				el1.fade('out');
				el2.fade('out');
				el1.get('morph').addEvent('complete', function() {
					el1.dispose();
					el2.dispose();
				});
			};
		el2.addEvent('click', function() {
			el1.destroy();
			el2.destroy();
		});
		clear.delay(1500);
		this.els.more = new Element('div', {
			'id': 'more'
		}).inject(this.element, 'inside');
		this.els.sendboxContainer = new Element('div', {
			'id': 'ape_sendbox_container'
		}).inject(this.els.more);
		this.els.sendBox = new Element('div', {
			'text': 'Say : ',
			'id': 'ape_sendbox'
		}).inject(this.els.sendboxContainer, 'bottom');
		this.els.sendbox = new Element('input', {
			'type': 'text',
			'id': 'sendbox_input',
			'autocomplete': 'off',
			'events': {
				'keypress': function(ev) {
					if (ev.code == 13) {
						var val = this.els.sendbox.get('value');
						if (val != '') {
							this.pipe.send(val);
							$(ev.target).set('value', '');
						}
					}
				}.bind(this)
			}
		}).inject(this.els.sendBox);
		this.els.sendButton = new Element('input', {
			'type': 'button',
			'id': 'sendbox_button',
			'value': 'Send',
			'events': {
				'click': function() {
					var val = this.els.sendbox.get('value');
					if (val != '') {
						this.pipe.send(val);
						$(ev.target).set('value', '');
					}
				}.bind(this)
			}
		}).inject(this.els.sendBox);
	},
	reset: function() {
		this.core.clearSession();
		if (this.element) {
			this.element.empty();
		}
		this.core.initialize(this.core.options);
	}

});