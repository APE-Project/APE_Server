<?php
	
	class ape_helloworld
	{
		public $socket = 11;

		public function __construct()
		{
			while(1);
		}

		public function adduser()
		{
			$this->socket++;
			echo 'Calling adduser...' . $this->socket . "\n";
		}		
	}
?>
