import socket
import time
import sys
import argparse
from collections import namedtuple
import traceback # Import traceback for better error reporting

# --- Test Nicknames ---
NICK1 = "Alice"
USER1 = "alice"
NICK2 = "Bob"
USER2 = "bob"
NICK3 = "Charlie"
USER3 = "charlie"
NICK_OP = "Operator" # For operator tests
USER_OP = "operator"
NICK_REG = "Regular" # For regular user tests
USER_REG = "regular"

# --- Test Channel ---
CHANNEL = "#42test"
CHANNEL_KEY = "mysecret" # For testing channel key mode

# --- ANSI Color Codes for Output ---
class colors:
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    ENDC = '\033[0m'

# A simple structure to parse IRC messages
Message = namedtuple('Message', 'prefix command params')

def parse_message(raw_msg):
    """Parses a raw IRC message and returns a Message named tuple."""
    parts = raw_msg.strip().split()
    if not parts:
        return None
    
    prefix = ''
    if parts[0].startswith(':'):
        prefix = parts.pop(0)[1:]

    command = parts.pop(0)
    
    params = []
    # Handle trailing parameter (starts with ':')
    # According to RFC 1459, the trailing parameter is always the last, and
    # takes precedence for remaining arguments.
    found_trailing = False
    for i, part in enumerate(parts):
        if part.startswith(':'):
            params.append(' '.join(parts[i:])[1:])
            found_trailing = True
            break
        else:
            params.append(part)
    
    # If no trailing parameter, the above loop would have exhausted parts, or
    # if it broke early due to a trailing parameter, `parts` is already consumed.
    # No further action needed for `params` construction here.
            
    return Message(prefix, command, params)


class IRCClient:
    """A simple IRC client for testing purposes."""

    def __init__(self, nickname, username):
        self.sock = None
        self.buffer = ""
        self.nickname = nickname
        self.username = username
        self.is_connected = False

    def connect(self, host, port, password):
        """Connects to the IRC server and performs initial registration."""
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.connect((host, port))
            self.log(f"Connected to {host}:{port}")

            self.send_raw(f"PASS {password}")
            self.send_raw(f"NICK {self.nickname}")
            self.send_raw(f"USER {self.username} 0 * :{self.nickname}")
            
            # Wait for the welcome message (RPL_WELCOME 001)
            for _ in range(15): # Try for a few moments, increase attempts for slow servers
                msg = self.receive_data()
                if msg and msg.command == '001':
                    self.is_connected = True
                    self.log("Successfully registered.")
                    self.clear_buffer() # Clear any remaining welcome messages (002, 003, 004, MOTD)
                    return True
                elif msg and msg.command in ['433', '461', '462', '464', '465']: # Nick in use, NeedMoreParams, AlreadyRegistered, PasswdMismatch, BannedFromServer
                     self.log(f"Registration failed: {msg.command} {' '.join(msg.params)}", 'error')
                     self.disconnect() # Close socket on failed registration
                     return False
            
            self.log("Registration failed: Did not receive welcome message or encountered an error.", 'error')
            self.disconnect() # Close socket if timeout
            return False

        except Exception as e:
            self.log(f"Connection failed: {e}", level='error')
            return False

    def disconnect(self, quit_message="Leaving"):
        """Disconnects from the server."""
        if self.sock: # Always attempt to close socket if it exists
            if self.is_connected: # Only send QUIT if successfully registered
                try:
                    self.send_raw(f"QUIT :{quit_message}")
                    # Give server time to process QUIT. Optionally wait for ERROR or close.
                    time.sleep(0.1) 
                except Exception as e:
                    self.log(f"Error sending QUIT: {e}", 'error')
            try:
                self.sock.shutdown(socket.SHUT_RDWR) # Attempt graceful shutdown
                self.sock.close()
                self.sock = None
                self.is_connected = False
                self.log("Disconnected and socket closed.")
            except Exception as e:
                self.log(f"Error closing socket: {e}", 'error')
        else:
            self.log("Socket was already closed or never opened.")


    def send_raw(self, data):
        """Sends a raw, CRLF-terminated message to the server."""
        if not self.sock:
            self.log("Socket not available to send raw data.", 'error')
            return
        try:
            message = data + "\r\n"
            self.sock.sendall(message.encode('utf-8'))
            print(f"{colors.BLUE}[SENT by {self.nickname}]> {data}{colors.ENDC}")
        except Exception as e:
            self.log(f"Failed to send data: {e}", level='error')
            self.is_connected = False # Assume connection lost on send error

    def send_partial(self, data):
        """Sends partial data without CRLF termination."""
        if not self.sock:
            self.log("Socket not available to send partial data.", 'error')
            return
        try:
            self.sock.sendall(data.encode('utf-8'))
            escaped_data = data.replace('\r', '\\r').replace('\n', '\\n')
            print(f"{colors.BLUE}[SENT PARTIAL by {self.nickname}]> {escaped_data}{colors.ENDC}")   
        except Exception as e:
            self.log(f"Failed to send partial data: {e}", level='error')
            self.is_connected = False # Assume connection lost on send error


    def receive_data(self, timeout=1.0):
        """
        Receives data from the socket, handles buffering, and returns one parsed message.
        Returns a Message object or None if no complete message is received.
        """
        if not self.sock:
            return None
        self.sock.settimeout(timeout)
        while "\r\n" not in self.buffer:
            try:
                data = self.sock.recv(4096)
                if not data:
                    self.log("Connection closed by server.", level='error')
                    self.is_connected = False
                    return None
                self.buffer += data.decode('utf-8')
            except socket.timeout:
                return None # No data received within the timeout
            except Exception as e:
                self.log(f"Receive error: {e}", level='error')
                self.is_connected = False
                return None
        
        line, self.buffer = self.buffer.split("\r\n", 1)
        print(f"{colors.YELLOW}[RECV by {self.nickname}]< {line}{colors.ENDC}")
        
        # Handle PING/PONG automatically
        if line.startswith("PING"):
            # Extract the PING argument correctly (after the colon if present, or first param)
            ping_token = line.split(' ')[1] if len(line.split(' ')) > 1 else ''
            if ping_token.startswith(':'):
                ping_token = ping_token[1:]
            self.send_raw(f"PONG :{ping_token}") # Standard PONG format
            return self.receive_data() # Recursively get the next real message

        return parse_message(line)

    def find_message(self, command, attempts=10, timeout_per_attempt=0.5, exact_params=None):
        """
        Waits for and returns a message with a specific command.
        Can optionally check for exact parameters.
        """
        for _ in range(attempts):
            msg = self.receive_data(timeout=timeout_per_attempt)
            if msg and msg.command == command:
                if exact_params:
                    # Check if all specified exact_params are present and match.
                    # This assumes exact_params is a list of strings.
                    if len(msg.params) >= len(exact_params) and \
                       all(msg.params[i] == exact_params[i] for i in range(len(exact_params))):
                        return msg
                else:
                    return msg
        return None
    
    def clear_buffer(self, timeout=0.1):
        """Clears any pending messages in the buffer."""
        count = 0
        while self.receive_data(timeout=timeout):
            count += 1
        if count > 0:
            self.log(f"Cleared {count} messages from buffer.", 'info')

    def log(self, message, level='info'):
        color = colors.GREEN if level == 'info' else colors.RED
        print(f"{color}[CLIENT {self.nickname}] {message}{colors.ENDC}")

# --- Test Functions ---

def run_test(test_func):
    """Decorator to run a test function and print its status."""
    def wrapper(host, port, password): # Pass arguments to the wrapped function
        test_name = test_func.__name__
        print(f"\n--- Running test: {test_name} ---")
        try:
            result = test_func(host, port, password) # Pass arguments to the test
            if result:
                print(f"{colors.GREEN}--- PASS: {test_name} ---{colors.ENDC}")
            else:
                print(f"{colors.RED}--- FAIL: {test_name} ---{colors.ENDC}")
            return result
        except Exception as e:
            print(f"{colors.RED}--- ERROR in {test_name}: {e} ---{colors.ENDC}")
            traceback.print_exc() # Print full traceback for errors
            return False
    return wrapper

@run_test
def test_connection_and_auth(host, port, password):
    """Tests if a client can connect and authenticate successfully."""
    client = IRCClient(NICK1, USER1)
    connected = client.connect(host, port, password)
    client.disconnect()
    return connected

@run_test
def test_invalid_password(host, port, password):
    """Tests if the server rejects a client with a wrong password."""
    client = IRCClient(NICK1, USER1)
    # The connect method will already log failure and set is_connected to False.
    connected = client.connect(host, port, "wrong_password")
    
    # We expect registration to fail. So, `connected` should be False.
    # The `connect` method already handles cleanup (disconnects if fails).
    return not connected

@run_test
def test_nick_collision(host, port, password):
    """Tests if the server prevents two users from having the same nickname."""
    client1 = IRCClient(NICK1, USER1)
    client2 = IRCClient(NICK1, USER2) # Same nick, different user

    if not client1.connect(host, port, password):
        client1.log("Client1 failed to connect, aborting test.", 'error')
        return False
    
    # Client2 should fail to register due to nick collision (e.g., ERR_NICKNAMEINUSE 433)
    client2_connected = client2.connect(host, port, password)
    
    # We expect client2 to have failed registration (not client2_connected)
    result = not client2_connected
    
    client1.disconnect()
    client2.disconnect()
    return result

@run_test
def test_join_part_channel(host, port, password):
    """Tests JOINing and PARTing a channel."""
    client = IRCClient(NICK1, USER1)
    if not client.connect(host, port, password):
        return False

    client.send_raw(f"JOIN {CHANNEL}")
    
    # Expect a JOIN confirmation from the server (echo of own JOIN)
    join_msg = client.find_message("JOIN", exact_params=[CHANNEL], attempts=5)
    if not join_msg:
        client.log("Did not receive own JOIN confirmation.", 'error')
        client.disconnect()
        return False
        
    # Expect RPL_NAMREPLY (353) and RPL_ENDOFNAMES (366)
    names_reply = client.find_message("353")
    end_of_names = client.find_message("366")
    if not (names_reply and end_of_names):
        client.log("Did not receive RPL_NAMREPLY or RPL_ENDOFNAMES.", 'error')
        client.disconnect()
        return False
        
    client.send_raw(f"PART {CHANNEL} :Leaving now")
    part_msg = client.find_message("PART", exact_params=[CHANNEL, "Leaving now"])
    if not part_msg:
        client.log("Did not receive PART confirmation.", 'error')
        client.disconnect()
        return False
    client.disconnect()
    return True
    
@run_test
def test_multi_channel_join(host, port, password):
    """Tests joining multiple channels in a single JOIN command."""
    client = IRCClient(NICK1, USER1)
    client_op = IRCClient(NICK_OP, USER1)
    
    if not client.connect(host, port, password):
        return False
    if not client_op.connect(host, port, password):
        client.disconnect()
        return False
    
    # First set up a channel with key and invite-only mode
    channel2 = "#test2"
    channel3 = "#test3"
    key2 = "key2"
    
    # Operator sets up channel2 with key
    client_op.send_raw(f"JOIN {channel2}")
    client_op.find_message("JOIN", exact_params=[channel2])
    client_op.send_raw(f"MODE {channel2} +k {key2}")
    client_op.find_message("MODE")
    
    # Operator sets up channel3 as invite-only
    client_op.send_raw(f"JOIN {channel3}")
    client_op.find_message("JOIN", exact_params=[channel3])
    client_op.send_raw(f"MODE {channel3} +i")
    client_op.find_message("MODE")
    
    # Test joining multiple channels with keys
    client.send_raw(f"JOIN {CHANNEL},{channel2} {CHANNEL_KEY},{key2}")
    
    # Should succeed for both channels
    join1 = client.find_message("JOIN", exact_params=[CHANNEL])
    join2 = client.find_message("JOIN", exact_params=[channel2])
    if not (join1 and join2):
        client.log("Failed to join channels with keys", 'error')
        client.disconnect()
        client_op.disconnect()
        return False
    
    # Try to join invite-only channel without invite
    client.send_raw(f"JOIN {channel3}")
    err_invite = client.find_message("473")  # ERR_INVITEONLYCHAN
    if not err_invite:
        client.log("Should have received ERR_INVITEONLYCHAN", 'error')
        client.disconnect()
        client_op.disconnect()
        return False
        
    # Try to join channel2 with wrong key
    client.send_raw(f"PART {channel2}")
    client.find_message("PART")
    client.send_raw(f"JOIN {channel2} wrongkey")
    err_key = client.find_message("475")  # ERR_BADCHANNELKEY
    if not err_key:
        client.log("Should have received ERR_BADCHANNELKEY", 'error')
        client.disconnect()
        client_op.disconnect()
        return False
    
    client.disconnect()
    client_op.disconnect()
    return True

@run_test
def test_channel_messaging(host, port, password):
    """Tests sending and receiving messages in a channel between two clients."""
    client1 = IRCClient(NICK1, USER1)
    client2 = IRCClient(NICK2, USER2)

    if not client1.connect(host, port, password): return False
    if not client2.connect(host, port, password): 
        client1.disconnect()
        return False

    client1.send_raw(f"JOIN {CHANNEL}")
    client1.clear_buffer() # Clear server's 353, 366 messages
    
    client2.send_raw(f"JOIN {CHANNEL}")
    client2.clear_buffer() # Clear server's 353, 366 messages

    # After both join, they might see each other's JOIN, clear buffers for clean state
    time.sleep(0.5) # Give some time for JOINs to propagate
    client1.clear_buffer()
    client2.clear_buffer()
    
    test_message = f"Hello from {client1.nickname}!"
    client1.send_raw(f"PRIVMSG {CHANNEL} :{test_message}")
    
    # Client2 should receive the message
    msg = client2.find_message("PRIVMSG", attempts=10)
    
    client1.disconnect()
    client2.disconnect()
    
    if not msg:
        print("FAIL: Client2 did not receive the message.")
        return False
    
    sender_nick = msg.prefix.split('!')[0]
    target = msg.params[0]
    content = msg.params[1]
    
    return sender_nick == client1.nickname and target == CHANNEL and content == test_message

@run_test
def test_private_messaging(host, port, password):
    """Tests sending a private message (user to user)."""
    client1 = IRCClient(NICK1, USER1)
    client2 = IRCClient(NICK2, USER2)

    if not client1.connect(host, port, password): return False
    if not client2.connect(host, port, password): 
        client1.disconnect()
        return False

    time.sleep(0.5) # Give server time to process connections
    client1.clear_buffer() # Clear any initial server messages
    client2.clear_buffer()
    
    test_message = "This is a private message."
    client1.send_raw(f"PRIVMSG {client2.nickname} :{test_message}")
    
    # Client2 should receive the message
    msg = client2.find_message("PRIVMSG", attempts=10)
    
    client1.disconnect()
    client2.disconnect()
    
    if not msg:
        print("FAIL: Client2 did not receive the private message.")
        return False
        
    sender_nick = msg.prefix.split('!')[0]
    target = msg.params[0]
    content = msg.params[1]
    
    return sender_nick == client1.nickname and target == client2.nickname and content == test_message

@run_test
def test_partial_message_sending(host, port, password):
    """Tests if the server correctly buffers and processes partial messages."""
    client = IRCClient(NICK1, USER1)
    client2 = IRCClient(NICK2, USER2)
    
    if not client.connect(host, port, password): return False
    if not client2.connect(host, port, password):
        client.disconnect()
        return False

    try:
        client.clear_buffer()
        client2.clear_buffer()
        client2.send_raw(f"JOIN {CHANNEL}")

        client.log("Testing partial JOIN command...", 'info')
        # Send JOIN command in two parts
        client.send_partial("JOIN ")
        time.sleep(0.1) # Simulate network delay
        client.send_partial(f"{CHANNEL}\r\n")
        
        # Expect JOIN confirmation, RPL_NAMREPLY (353), and RPL_ENDOFNAMES (366)
        join_msg = client.find_message("JOIN", exact_params=[CHANNEL], attempts=5)
        names_reply = client.find_message("353")
        end_of_names = client.find_message("366")
        
        if not (join_msg and names_reply and end_of_names):
            client.log("Partial JOIN test failed: Did not receive expected responses.", 'error')
            return False
        
        # Ensure client2 sees client1 join
        client2_sees_join = client2.find_message("JOIN", exact_params=[CHANNEL], attempts=5)
        if not client2_sees_join:
            client.log("Partial JOIN test failed: Client2 did not see Client1 join.", 'error')
            return False

        client.send_raw(f"PART {CHANNEL}") # Clean up
        client.find_message("PART")
        client.clear_buffer()
        client2.clear_buffer()

        client.log("Testing partial PRIVMSG command...", 'info')
        partial_msg_content = "This is a partial message test."
        
        # Send PRIVMSG in multiple parts
        client.send_partial("PRIVM")
        time.sleep(0.1)
        client.send_partial("SG ")
        time.sleep(0.1)
        client.send_partial(f"{NICK2} ")
        time.sleep(0.1)
        client.send_partial(f":{partial_msg_content}\r\n")
        
        # Client2 should receive the message
        received_msg = client2.find_message("PRIVMSG", attempts=10)
        
        if not received_msg:
            client.log("Partial PRIVMSG test failed: Client2 did not receive the message.", 'error')
            return False
        
        sender_nick = received_msg.prefix.split('!')[0]
        target = received_msg.params[0]
        content = received_msg.params[1]
        
        if not (sender_nick == client.nickname and target == NICK2 and content == partial_msg_content):
            client.log(f"Partial PRIVMSG test failed: Message content mismatch. Expected '{partial_msg_content}', got '{content}'", 'error')
            return False

        return True
    finally:
        cleanup_clients(client, client2)


# Helper to setup a channel with an operator and a regular user
def setup_channel_with_roles(host, port, password, channel_name):
    op_client = IRCClient(NICK_OP, USER_OP)
    reg_client = IRCClient(NICK_REG, USER_REG)

    if not op_client.connect(host, port, password):
        op_client.log("Operator client failed to connect.", 'error')
        return None, None
    if not reg_client.connect(host, port, password):
        reg_client.log("Regular client failed to connect.", 'error')
        op_client.disconnect()
        return None, None
    
    op_client.send_raw(f"JOIN {channel_name}")
    op_client.clear_buffer() # Clear 353, 366 and own JOIN echo

    reg_client.send_raw(f"JOIN {channel_name}")
    reg_client.clear_buffer() # Clear 353, 366 and own JOIN echo

    time.sleep(0.5) # Give server time to process JOINs and establish roles
    op_client.clear_buffer() # Clear any notifications of REG_CLIENT joining
    reg_client.clear_buffer() # Clear any notifications of OP_CLIENT joining (if seen)

    # Basic verification of op status (by checking names list for @ or sending MODE and parsing)
    # Most servers assign op to first joiner. Let's send MODE to see
    op_client.send_raw(f"MODE {channel_name}")
    mode_is_msg = op_client.find_message("324", attempts=3) # RPL_CHANNELMODEIS
    if mode_is_msg and len(mode_is_msg.params) >= 3 and mode_is_msg.params[2]:
        op_client.log(f"Operator channel modes: {mode_is_msg.params[2]}", 'info')
    else:
        op_client.log("Could not confirm operator's channel modes (324 not received or empty). Proceeding assuming first joiner is op.", 'yellow')

    return op_client, reg_client

# Helper to clean up clients
def cleanup_clients(*clients):
    for client in clients:
        if client:
            client.disconnect()


# --- Tests for restricted commands ---

@run_test
def test_normal_user_kick(host, port, password):
    """Tests if a regular user can KICK someone from a channel."""
    op_client, reg_client = setup_channel_with_roles(host, port, password, CHANNEL)
    if not op_client or not reg_client: return False

    try:
        op_client.log(f"Regular user ({reg_client.nickname}) attempting to KICK operator ({op_client.nickname}).", 'info')
        reg_client.send_raw(f"KICK {CHANNEL} {NICK_OP} :You're out!")
        
        # Expect ERR_CHANOPRIVSNEEDED (482) for regular user
        error_msg = reg_client.find_message("482", attempts=5)
        
        # Additionally, verify op_client is NOT kicked (still in channel)
        # One way is to send a message from op_client and reg_client receives it.
        op_client.clear_buffer()
        reg_client.clear_buffer()

        op_client.send_raw(f"PRIVMSG {CHANNEL} :Still here after kick attempt!")
        op_msg_received_by_reg = reg_client.find_message("PRIVMSG", attempts=5)

        if not error_msg:
            reg_client.log("FAIL: Regular user did not receive ERR_CHANOPRIVSNEEDED (482) when trying to KICK.", 'error')
            return False
        if not op_msg_received_by_reg:
            reg_client.log("FAIL: Operator client was unexpectedly kicked or message not propagated (message not received by regular user).", 'error')
            return False
        
        reg_client.log("SUCCESS: Regular user was prevented from kicking.", 'info')
        return True
    finally:
        cleanup_clients(op_client, reg_client)

@run_test
def test_normal_user_invite(host, port, password):
    """Tests if a regular user can INVITE someone to a channel."""
    op_client, reg_client = setup_channel_with_roles(host, port, password, CHANNEL)
    if not op_client or not reg_client: return False

    client_not_in_channel = IRCClient(NICK3, USER3)
    if not client_not_in_channel.connect(host, port, password):
        reg_client.log("Client not in channel failed to connect.", 'error')
        cleanup_clients(op_client, reg_client)
        return False

    try:
        reg_client.log(f"Regular user ({reg_client.nickname}) attempting to INVITE {NICK3} to {CHANNEL}.", 'info')
        reg_client.send_raw(f"INVITE {NICK3} {CHANNEL}")
        
        # Expect ERR_CHANOPRIVSNEEDED (482) for regular user
        error_msg = reg_client.find_message("482", attempts=5)

        # Ensure NICK3 does NOT receive an INVITE message (e.g. 341 RPL_INVITING or direct INVITE command)
        invite_confirmation = client_not_in_channel.find_message("341", attempts=2) # RPL_INVITING
        invite_cmd_received = client_not_in_channel.find_message("INVITE", attempts=2) # Direct INVITE
        
        if not error_msg:
            reg_client.log("FAIL: Regular user did not receive ERR_CHANOPRIVSNEEDED (482) when trying to INVITE.", 'error')
            return False
        if invite_confirmation or invite_cmd_received:
            reg_client.log("FAIL: Client not in channel received an INVITE confirmation.", 'error')
            return False
        
        reg_client.log("SUCCESS: Regular user was prevented from inviting.", 'info')
        return True
    finally:
        cleanup_clients(op_client, reg_client, client_not_in_channel)

@run_test
def test_normal_user_topic_set(host, port, password):
    """Tests if a regular user can change the TOPIC of a channel (when +t mode is on)."""
    op_client, reg_client = setup_channel_with_roles(host, port, password, CHANNEL)
    if not op_client or not reg_client: return False

    try:
        # Operator first sets a topic (assuming +t is default or set by server)
        initial_topic = "Initial Topic by Operator"
        op_client.log(f"Operator ({op_client.nickname}) setting initial TOPIC: {initial_topic}", 'info')
        op_client.send_raw(f"TOPIC {CHANNEL} :{initial_topic}")
        # Server typically echoes RPL_TOPIC (332) to the setter and PROPAGATES TOPIC to channel members
        op_client.find_message("332", attempts=5) # RPL_TOPIC for setting
        reg_client.find_message("TOPIC", attempts=5) # Direct TOPIC command or 332
        op_client.clear_buffer()
        reg_client.clear_buffer() 
        
        new_topic_by_reg = "Topic by Regular User"
        reg_client.log(f"Regular user ({reg_client.nickname}) attempting to set TOPIC: {new_topic_by_reg}", 'info')
        reg_client.send_raw(f"TOPIC {CHANNEL} :{new_topic_by_reg}")
        
        # Expect ERR_CHANOPRIVSNEEDED (482) for regular user
        error_msg = reg_client.find_message("482", attempts=5)
        
        # Verify topic was NOT changed by checking with operator
        op_client.send_raw(f"TOPIC {CHANNEL}")
        actual_topic_msg = op_client.find_message("332", attempts=5) # RPL_TOPIC (for viewing)

        if not error_msg:
            reg_client.log("FAIL: Regular user did not receive ERR_CHANOPRIVSNEEDED (482) when trying to set TOPIC.", 'error')
            return False
        
        if not (actual_topic_msg and actual_topic_msg.params[2] == initial_topic):
            reg_client.log(f"FAIL: Topic was changed unexpectedly to '{actual_topic_msg.params[2] if actual_topic_msg else 'N/A'}'. Expected '{initial_topic}'.", 'error')
            return False
        
        reg_client.log("SUCCESS: Regular user was prevented from setting topic.", 'info')
        return True
    finally:
        cleanup_clients(op_client, reg_client)

@run_test
def test_normal_user_mode_change(host, port, password):
    """Tests if a regular user can change channel MODE (i, t, k, o, l)."""
    op_client, reg_client = setup_channel_with_roles(host, port, password, CHANNEL)
    if not op_client or not reg_client: return False

    try:
        # Operator sets initial modes, including +t (topic restricted to ops) which is default on many servers
        op_client.send_raw(f"MODE {CHANNEL} +t")
        op_client.clear_buffer()
        reg_client.clear_buffer()

        modes_to_test = [
            "+i", "-i",  # Invite-only
            "+t", "-t",  # Topic restriction
            f"+k {CHANNEL_KEY}", f"-k {CHANNEL_KEY}", # Channel key (requires setting/unsetting with key)
            f"+o {NICK_REG}", f"-o {NICK_OP}", # Give/take op (regular user trying to give/take op)
            "+l 5", "-l" # User limit
        ]
        
        success = True
        for mode_str in modes_to_test:
            reg_client.log(f"Regular user ({reg_client.nickname}) attempting MODE {CHANNEL} {mode_str}...", 'info')
            reg_client.send_raw(f"MODE {CHANNEL} {mode_str}")
            
            error_msg = reg_client.find_message("482", attempts=5) # ERR_CHANOPRIVSNEEDED
            
            if not error_msg:
                reg_client.log(f"FAIL: Regular user did not receive ERR_CHANOPRIVSNEEDED (482) for MODE {mode_str}.", 'error')
                success = False
                break
            reg_client.log(f"SUCCESS: Regular user received 482 for MODE {mode_str}.", 'info')
            
            # Additional verification: Check that the mode was NOT actually applied
            # This is complex for all modes. For simplicity, we rely on 482 as the primary check.
            # A good server should not apply a mode if it returns 482.
            op_client.clear_buffer() # Clear any spurious MODE messages
            reg_client.clear_buffer()

        return success
    finally:
        cleanup_clients(op_client, reg_client)


def main():
    """Parses arguments and runs all defined tests."""
    parser = argparse.ArgumentParser(description="IRC Server Test Suite for ft_irc.")
    parser.add_argument('--host', type=str, default="127.0.0.1",
                        help="The IP address of the IRC server (default: 127.0.0.1)")
    parser.add_argument('--port', type=int, default=6667,
                        help="The port of the IRC server (default: 6667)")
    parser.add_argument('--password', type=str, required=True,
                        help="The password for the IRC server")

    args = parser.parse_args()

    host = args.host
    port = args.port
    password = args.password

    # Ensure the ft_irc server is running before starting the tests.
    print(f"Starting tests for IRC server at {host}:{port} with password '{password}'")
    
    tests = [
        test_connection_and_auth,
        test_invalid_password,
        test_nick_collision,
        test_join_part_channel,
        test_channel_messaging,
        test_private_messaging,
        test_partial_message_sending,
        test_normal_user_kick,       
        test_normal_user_invite,     
        test_normal_user_topic_set,  
        test_normal_user_mode_change, 
        test_multi_channel_join
    ]
    
    passed_count = 0
    for test in tests:
        # Pass the parsed host, port, and password to each test function
        if test(host, port, password): 
            passed_count += 1
            
    print("\n--- Test Summary ---")
    print(f"Total tests: {len(tests)}")
    print(f"{colors.GREEN}Passed: {passed_count}{colors.ENDC}")
    print(f"{colors.RED}Failed: {len(tests) - passed_count}{colors.ENDC}")
    
    if passed_count != len(tests):
        sys.exit(1) # Exit with a non-zero code if any test failed
    sys.exit(0)

if __name__ == "__main__":
    main()