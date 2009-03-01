<?php
	
	class ape_helloworld
	{
		private $socket;

		public function __construct()
		{
			$this->socket = 'done';
			// Called at startup
			echo 'aaa' . "\n";
			
		}

		public function adduser()
		{
			echo 'Sock : ' . $this->socket . "\n"; 
		}		
	}
?>
