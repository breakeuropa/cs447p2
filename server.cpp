#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <system_error>
#include <fstream> 
#include <sstream>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <random>
#include <filesystem>

// C headers for socket API
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>

#define BACKLOG 10
constexpr size_t kMaxLineLength = 4096;

constexpr const char* kReplyReady         = "220 Service ready.";
constexpr const char* kReplyClosing       = "221 Service closing control connection.";
constexpr const char* kReplyLoggedIn      = "230 User logged in, proceed.";
constexpr const char* kReplyAuthFailed    = "530 Authentication failed.";
constexpr const char* kReplyNotLoggedIn   = "530 Not logged in.";
constexpr const char* kReplyUnrecognized  = "500 Syntax error, command unrecognized.";
constexpr const char* kReplyBadArgs       = "501 Syntax error in parameters or arguments.";

struct ServerState
{
	std::mutex mutex;
	int port = 0;
	int peer_interval = 0;
	std::string peer_subnet;
};

struct Session
{
	int fd = -1;
	int data_listen_fd = -1;
	bool logged_in = false;
	std::string client_ip;
	std::string buffer;
	std::string cwd = "/";
	bool is_peer = false;
};

void *get_in_addr(struct sockaddr *sa) 
{
	if (sa->sa_family == AF_INET) 
	{
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

bool send_reply(int client_fd, const std::string& reply)
{
	std::string msg = reply + "\r\n";
	size_t sent = 0;
	while (sent < msg.size())
	{
		ssize_t n = send(client_fd, msg.c_str() + sent, msg.size() - sent, MSG_NOSIGNAL);
		if (n == -1)
		{
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	return true;
}

std::string format_list_line(const std::filesystem::path& full_path, const std::string& name)
{
	struct stat st;
	if (stat(full_path.c_str(), &st) == -1)
	{
		return "";
	}

	bool is_dir = S_ISDIR(st.st_mode);
	std::string perms = is_dir ? "drwxr-xr-x" : "-rw-r--r--";
	std::string size  = is_dir ? "0" : std::to_string(st.st_size);

	struct tm tm_buf;
	localtime_r(&st.st_mtime, &tm_buf);
	char date[32];
	strftime(date, sizeof date, "%b %d %H:%M", &tm_buf);

	return perms + " 1 local " + size + " " + date + " " + name + "\r\n";
}

bool send_all(int fd, const char* data, size_t len)
{
	size_t sent = 0;
	while (sent < len)
	{
		ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
		if (n == -1)
		{
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	return true;
}

int accept_data_connection(Session& session)
{
	if (session.data_listen_fd == -1)
	{
		send_reply(session.fd, "425 Can't open data connection.");
		return -1;
	}

	struct pollfd pfd;
	pfd.fd = session.data_listen_fd;
	pfd.events = POLLIN;

	int ready=poll(&pfd, 1, 10000);
	if (ready <=0)
	{
		close(session.data_listen_fd);
		session.data_listen_fd = -1;
		send_reply(session.fd, "425 Can't open data connection.");
		return -1;
	}
	
	int data_fd = accept(session.data_listen_fd, NULL, NULL);
	close(session.data_listen_fd);
	session.data_listen_fd = -1;
	
	if (data_fd == -1)
	{
		send_reply(session.fd, "425 Can't open data connection.");
	}

	return data_fd;
}

std::string to_upper(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
	return s;
}

bool read_line(Session& session, std::string& line)
{
	char buf[1024];
	while (true)
	{
		size_t pos = session.buffer.find('\n');
		if (pos != std::string::npos)
		{
			line = session.buffer.substr(0, pos);
			session.buffer.erase(0, pos + 1);
			if (!line.empty() && line.back() == '\r')
			{
				line.pop_back();
			}
			return true;
		}
		if (session.buffer.size() > kMaxLineLength)
		{
			return false;
		}
		int numbytes = recv(session.fd, buf, sizeof(buf), 0);
		if (numbytes == -1)
		{
			perror("recv");
			return false;
		}
		if (numbytes == 0)
		{
			return false;
		}
		session.buffer.append(buf, numbytes);
		
	}
}

bool resolve_path(const std::string& cwd, const std::string& arg, std::string& result)
{
	std::vector<std::string> parts;
	std::string piece;

	if (arg.empty() || arg[0] != '/')
	{
		std::istringstream cwd_stream(cwd);
		while (std::getline(cwd_stream, piece, '/'))
		{
			if (!piece.empty())
			{
				parts.push_back(piece);
			}
		}
	}

	std::istringstream arg_stream(arg);
	while (std::getline(arg_stream, piece, '/'))
	{
		if (piece.empty() || piece == ".")
		{
			continue;
		}
		else if (piece == "..")
		{
			if (parts.empty())
			{
				return false;
			}
			else
			{
				parts.pop_back();
			}
		}
		else
		{
			parts.push_back(piece);
		}
	}

	result = "";
	for (const std::string& p : parts)
	{
		result += "/" + p;
	}
	if (result.empty())
	{
		result = "/";
	}

	return true;
}

std::string to_real_path(const std::string& virtual_path)
{
	if (virtual_path == "/")
	{
		return "db";
	}
	return "db" + virtual_path;
}

int make_listener(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1)
	{
		perror("make_listener: socket");
		return -1;
	}

	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == -1)
	{
		perror("make_listener: bind");
		close(fd);
		return -1;
	}
	
    	if (listen(fd, BACKLOG) == -1) 
    	{
        	perror("make_listener: listen");
       		close(fd);
		return -1;
    	}

	return fd;
}

void handle_list(Session &session, const std::string& args)
{
	if (session.data_listen_fd == -1)
	{
		send_reply(session.fd, "425 Can't open data connection.");
		return;
	}
	
	std::string path;
	if (args.empty())
	{
		path = session.cwd;
	}
	else 
	{
		if (!resolve_path(session.cwd, args, path))
		{
			send_reply(session.fd, "550 Requested action not taken.");
			return;
		}
	}
	
	std::string real = to_real_path(path);
	if (!std::filesystem::is_directory(real))
	{
		send_reply(session.fd, "550 Requested action not taken.");
		return;
	}

	std::vector<std::string> names;
	try
	{
		for (const auto& entry : std::filesystem::directory_iterator(real))
		{
			names.push_back(entry.path().filename().string());
		}
	}
	catch (const std::filesystem::filesystem_error& e)
	{
		std::cerr << "list: " << e.what() << std::endl;
		send_reply(session.fd, "550 Requested action not taken.");
		return;
	}
	std::sort(names.begin(), names.end());

	int data_fd = accept_data_connection(session);
	if (data_fd == -1)
	{
        	return;
    	}
	send_reply(session.fd, "150 File status okay; about to open data connection.");

	bool ok = true;
	for (const std::string& name : names)
	{
		std::string line = format_list_line(real + "/" + name, name);
		if (line.empty())
		{
			continue;
		}
		if (!send_all(data_fd, line.c_str(), line.size()))
		{
			ok = false;
			break;
		}
	}
	close(data_fd);

	if (ok)
	{
		send_reply(session.fd, "226 Directory send OK.");
	}
	else
	{
		send_reply(session.fd, "426 Connection closed; transfer aborted.");
	}
}

void handle_retr(Session& session, const std::string& args)
{
	if (args.empty())
	{
		send_reply(session.fd, kReplyBadArgs);
		return;
	}
	if (session.data_listen_fd == -1)
	{
		send_reply(session.fd, "425 Can't open data connection.");
		return;
	}

	std::string path;
	if (!resolve_path(session.cwd, args, path))
	{
		send_reply(session.fd, "550 Requested action not taken.");
		return;
	}

	std::string real = to_real_path(path);
	if (!std::filesystem::is_regular_file(real))
	{
		send_reply(session.fd, "550 Requested action not taken.");
		return;
	}

	std::ifstream file(real, std::ios::binary);
	if (!file)
	{
		send_reply(session.fd, "550 Requested action not taken.");
		return;
	}

	int data_fd = accept_data_connection(session);
	if (data_fd == -1)
	{
		return;
	}

	send_reply(session.fd, "150 Opening data connection.");

	char buf[4096];
	bool ok = true;
	while (file.read(buf, sizeof buf) || file.gcount() > 0)
	{
		if (!send_all(data_fd, buf, static_cast<size_t>(file.gcount())))
		{
			ok = false;
			break;
		}
	}
	close(data_fd);

	if (ok)
	{
		send_reply(session.fd, "226 Transfer complete.");
	}
	else
	{
		send_reply(session.fd, "426 Connection closed; transfer aborted.");
	}
}

void handle_pasv(Session& session)
{
	if (session.data_listen_fd != -1)
	{
		close(session.data_listen_fd);
		session.data_listen_fd = -1;
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
    	if (fd == -1)
    	{
        	perror("pasv: socket");
        	send_reply(session.fd, "425 Can't open data connection.");
        	return;
    	}
	
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(0); 

	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == -1)
    	{
        	perror("pasv: bind");
        	close(fd);
        	send_reply(session.fd, "425 Can't open data connection.");
        	return;
    	}
	if (listen(fd, 1) == -1)
    	{	
        	perror("pasv: listen");
        	close(fd);
        	send_reply(session.fd, "425 Can't open data connection.");
        	return;
    	}

	struct sockaddr_in bound;
    	socklen_t bound_len = sizeof bound;
    	getsockname(fd, (struct sockaddr *)&bound, &bound_len);
	int port = ntohs(bound.sin_port);

	struct sockaddr_in local;
    	socklen_t local_len = sizeof local;
    	getsockname(session.fd, (struct sockaddr *)&local, &local_len);

	char ipbuf[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &local.sin_addr, ipbuf, sizeof ipbuf);
	std::string ip_text = ipbuf;
	std::replace(ip_text.begin(), ip_text.end(), '.', ',');
	std::string p1 = std::to_string(port / 256);
	std::string p2 = std::to_string(port % 256);
	
	session.data_listen_fd = fd;
	send_reply(session.fd, "227 Entering Passive Mode (" + ip_text + "," + p1 + "," + p2 + ")");
}

void handle_command(Session& session, const std::string& command, const std::string& args)
{
	if (command == "PWD")
	{
		send_reply(session.fd, "257 \"" + session.cwd + "\" is current directory");
	}
	else if (command == "CWD")
	{
		if (args.empty())
		{
			send_reply(session.fd, kReplyBadArgs);
		}
		else
		{
			std::string new_cwd;
			if (resolve_path(session.cwd, args, new_cwd) && std::filesystem::is_directory(to_real_path(new_cwd)))
			{
				session.cwd = new_cwd;
				send_reply(session.fd, "250 Directory changed.");
			}
			else
			{
				send_reply(session.fd, "550 Failed to change directory.");
			}
		}
	}
	else if (command == "CDUP")
	{
		if (!args.empty())
		{
			send_reply(session.fd, kReplyBadArgs);
		}
		else
		{
			std::string new_cwd;
			if (resolve_path(session.cwd, "..", new_cwd) && std::filesystem::is_directory(to_real_path(new_cwd)))
			{
				session.cwd = new_cwd;
				send_reply(session.fd, "200 Directory changed.");
			}
			else
			{
				send_reply(session.fd, "550 Failed to change directory.");
			}
		}
	}
	else if (command == "PASV")
	{
		if (!args.empty())
		{
			send_reply(session.fd, kReplyBadArgs);
		}
		else
		{
			handle_pasv(session);
		}
	}
	else if (command == "RETR")
	{
		handle_retr(session, args);
	}
	else if (command == "LIST")
	{
		handle_list(session, args);
	}
	else
	{
		send_reply(session.fd, kReplyUnrecognized);
	}
}

void handle_peer(int fd, struct sockaddr_storage their_addr, ServerState& state)
{
	(void)state;
	char ip[INET6_ADDRSTRLEN];
	inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), ip, sizeof ip);
	std::cout << "server: peer connection from " << ip << std::endl;

	Session session;
	session.fd = fd;
	session.client_ip = ip;
	std::string line;

	send_reply(fd, kReplyReady);

	while (read_line(session, line))
	{
		std::istringstream iss(line);
		std::string command, args;
		iss >> command;
		std::getline(iss, args);

		command = to_upper(command);
		size_t start = args.find_first_not_of(' ');
		args = (start == std::string::npos) ? "" : args.substr(start);

		if (command == "QUIT")
		{
			send_reply(fd, kReplyClosing);
			break;
		}
		else if (command == "USER")
		{
			if (args.rfind("peer@", 0) !=0)
			{
				send_reply(fd, kReplyAuthFailed);
				break;
			}

			std::string claimed = args.substr(5);
			if (claimed == session.client_ip)
			{
				session.logged_in = true;
				send_reply(fd, "230 Peer authenticated, proceed.");
			}
			else
			{
				send_reply(fd, kReplyAuthFailed);
				break;
			}
		}
		else if (!session.logged_in)
		{
			send_reply(fd, kReplyNotLoggedIn);
		}
		else
		{
			handle_command(session, command, args);
		}
	}

	if (session.data_listen_fd != -1)
	{
		close(session.data_listen_fd);
	}
	close(fd);
	std::cout << "server: peer " << ip << " disconnected" << std::endl;
}

void peer_listener(ServerState& state)
{
	int peer_fd = make_listener(state.port + 200);
	if (peer_fd == -1)
	{
		std::cerr << "peer listener: could not start" << std::endl;
		return;
	}
	std::cout << "server: waiting for peers on port " << state.port + 200 << "..." << std::endl;

	while (true)
	{
		struct sockaddr_storage their_addr; 
		socklen_t sin_size = sizeof their_addr;
		int fd = accept(peer_fd, (struct sockaddr *)&their_addr, &sin_size);
		if (fd == -1)
		{
			perror("peer accept");
			continue;
		}
		std::thread(handle_peer, fd, their_addr, std::ref(state)).detach();
	}
}

void handle_client(int client_fd, struct sockaddr_storage their_addr, ServerState& state)
{
	(void)state;
	char ip[INET6_ADDRSTRLEN];
	inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), ip, sizeof ip);
	std::cout << "server: got TCP connection from " << ip << std::endl;

	Session session;
	session.fd = client_fd;
	session.client_ip = ip;
	std::string line;

	send_reply(client_fd, kReplyReady);

	while (read_line(session, line))
	{
		std::istringstream iss(line);
		std::string command, args;
		iss >> command;
		std::getline(iss, args);

		command = to_upper(command);
		size_t start = args.find_first_not_of(' ');
		args = (start == std::string::npos) ? "" : args.substr(start);

		if (command.empty())
		{
			send_reply(client_fd, kReplyUnrecognized);
		}
		else if (command == "QUIT")
		{
			if (!args.empty())
			{
				send_reply(client_fd, kReplyBadArgs);
			}
			else
			{
				send_reply(client_fd, kReplyClosing);
				break;
			}
		}
		else if (command == "USER")
		{
			if (args.empty())
			{
				send_reply(client_fd, kReplyBadArgs);
			}
			else if (args == "anonymous")
			{
				session.logged_in = true;
				send_reply(client_fd, kReplyLoggedIn);
			}
			else
			{
				send_reply(client_fd, kReplyAuthFailed);
				break;
			}
		}
		else if (!session.logged_in)
		{
			send_reply(client_fd, kReplyNotLoggedIn);
		}
		else
		{
			handle_command(session, command, args);
		}
	}

	if (session.data_listen_fd != -1)
	{
		close(session.data_listen_fd);
	}

	close(client_fd);
	std::cout << "closed connection with " << ip << std::endl;
}

int main(int argc, char *argv[]) {
	/*
	int listen_fd, client_fd;
    	struct addrinfo hints, *servinfo, *p;
    	struct sockaddr_storage their_addr;
    	socklen_t sin_size;
    	int yes = 1;
    	int rv;
	*/
    	ServerState state;

	if (argc < 2) 
	{
		std::cerr << "Usage: " << argv[0] << " <server.conf>" << std::endl;
        	return 1;
	}

	int port = 0;
	int peer_interval = 0;
	std::string peer_subnet = "";

	std::ifstream conf_file(argv[1]);
	if (!conf_file.is_open())
    	{
		std::cerr << "Error: config file could not open." << std::endl;
		return 1;
    	}

    	std::string line;
    	while(std::getline(conf_file, line))
    	{
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}

		if(line.empty()) continue;

	    	size_t eq_pos = line.find('=');
	    	if (eq_pos == std::string::npos) continue;

	    	std::string key = line.substr(0, eq_pos);
	    	std::string value = line.substr(eq_pos + 1);

	    	if (key == "PORT")
	    	{
	    		if (!value.empty())
			{
				port = std::stoi(value);
			}
	    	}
	    	else if (key == "PEER_INTERVAL")
	    	{
	    		if (!value.empty())
			{
				peer_interval = std::stoi(value);
			}
	    	}
        	else if (key == "PEER_SUBNET")
        	{
         		peer_subnet = value;
		}
    	}

    	if (port == 0) 
    	{
        	std::cerr << "Error: PORT missing from config\n";
        	return 1;
    	}

	state.port = port;
	state.peer_interval = peer_interval;
	state.peer_subnet = peer_subnet;

    	conf_file.close();
	
	int listen_fd = make_listener(port);
	if (listen_fd == -1)
	{
		return 1;
	}

    	std::cout << "server: waiting for connections on port " << port << "..." << std::endl;
	std::jthread peer_thread(peer_listener, std::ref(state));
    	std::vector<std::jthread> threads; 
	struct sockaddr_storage their_addr;
	socklen_t sin_size;

    	while (true) 
    	{
        	sin_size = sizeof their_addr;
        	int client_fd = accept(listen_fd, (struct sockaddr *)&their_addr, &sin_size);

        	if (client_fd == -1) 
		{
            		perror("accept");
            		continue;
        	}
        	threads.push_back(std::jthread(handle_client, client_fd, their_addr, std::ref(state)));
    	}

    	close(listen_fd);
    	return 0;
}
