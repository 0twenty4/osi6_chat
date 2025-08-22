#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <ranges>
#include <fstream>
#include <iomanip>
#include <codecvt>
#include <ifaddrs.h>
#include <optional>
#include <queue>
#include <condition_variable>


enum {
	CLIENT_EXIT = 0,
	CLIENT_REGISTER,
	CLIENT_SCROLL_UP,
	CLIENT_SCROLL_DOWN,
	ERR,
	SUCCESS
};

struct CLIENT {
	int socket;

	std::string name;
	std::string ip;
	std::string port;

	bool is_scrolled_up = false;

	int window_scroll_offset = 0;

	CLIENT(int socket, const char* ip, const std::string& port) : socket(socket), ip(ip), port(port) {};
};

struct SERVER {
	int socket;
	sockaddr_in sockaddr{};

	std::unordered_map<int, CLIENT> clients;
	std::vector<std::string> chat_messages;
	std::unique_ptr<std::mutex> send_message_mutex = std::make_unique<std::mutex>();
	std::unique_ptr<std::mutex> receiving_message_mutex = std::make_unique<std::mutex>();

	int message_count = 0;
	int display_message_count = 20;
};

struct LOGGER {
	std::queue<std::string> logs;
	std::unique_ptr<std::mutex> log_mutex = std::make_unique<std::mutex>();
	std::unique_ptr<std::condition_variable> cv = std::make_unique<std::condition_variable>();
};

std::optional<SERVER> server;
LOGGER logger;

void log(const std::string& log_message) {
	std::lock_guard log_mutex_lock(*logger.log_mutex);
	logger.logs.push(log_message);
	logger.cv->notify_one();
}

void logging() {
	std::unique_lock log_mutex_lock(*logger.log_mutex);
	while (true) {
		logger.cv->wait(log_mutex_lock, [] {return !logger.logs.empty(); });

		while (!logger.logs.empty()) {
			std::string message = logger.logs.front();
			logger.logs.pop();

			std::cout << message << "\n";
		}

	}
}

int send_message_to_sender(std::string message, CLIENT& client) {
	int ret_val = send(client.socket, message.data(), message.size(), 0);

	if (ret_val < 0) {
		perror("send to sender");
		close(client.socket);
		return ERR;
	}

	return SUCCESS;
}

int send_message_to_all(std::string message, CLIENT* except_client = nullptr) {
	int ret_val;

	for (const auto& client : std::views::values(server->clients)) {

		if (&client == except_client or client.is_scrolled_up)
			continue;

		ret_val = send(client.socket, message.data(), message.size(), 0);

		if (ret_val < 0) {
			perror("send to all");
			close(client.socket);
			return 0;
		}
	}

	return SUCCESS;
}

int handle_command_message(const std::string& command_message, CLIENT& client) {
	std::string command;
	std::istringstream command_stream(command_message);

	command_stream >> command;

	if (command == "/register") {
		command_stream >> client.name;

		std::string chat_message_index = std::to_string(static_cast<int>(server->chat_messages.size()));
		std::string chat_message = chat_message_index +
			" Client " + client.name + "(" + client.ip + ":" + client.port + ") has joined the chat";

		server->chat_messages.push_back(chat_message);
		send_message_to_all(chat_message);

		server->message_count++;

		return CLIENT_REGISTER;
	}

	if (command == "/scroll_up") {
		int client_oldest_chat_message_index;
		command_stream >> client_oldest_chat_message_index;

		if (client_oldest_chat_message_index!=0)
			send_message_to_sender("/scroll_up " + server->chat_messages.at(client_oldest_chat_message_index-1), client);

		if (!client.is_scrolled_up)
			client.is_scrolled_up = true;

		return CLIENT_SCROLL_UP;
	}

	if (command == "/scroll_down") {
		int client_newest_chat_message_index;
		command_stream >> client_newest_chat_message_index;
		
		if (client_newest_chat_message_index!=static_cast<int>(server->chat_messages.size())-1) 
			send_message_to_sender("/scroll_down " + server->chat_messages[client_newest_chat_message_index+1], client);
		
		bool is_last_message_sent =
			client_newest_chat_message_index + 1 == static_cast<int>(server->chat_messages.size()) - 1;
		if (is_last_message_sent)
			client.is_scrolled_up = false;

		return CLIENT_SCROLL_DOWN;
	}

	if (command == "/exit") {
		std::string chat_message_index = std::to_string(static_cast<int>(server->chat_messages.size()));
		std::string chat_message =chat_message_index +
			"Client " + client.name + " (" + client.ip + ":" + client.port + ") has left the chat";

		server->chat_messages.push_back(chat_message);
		send_message_to_all(chat_message, &client);

		server->message_count++;

		log("Client " + client.ip + ":" + client.port + " has disconnected");

		server->clients.erase(client.socket);
		close(client.socket);

		return CLIENT_EXIT;
	}

	send_message_to_sender("Unknown command", client);

	return 0;
}

int handle_message(std::string& message, CLIENT& client) {
	if (message.at(0) == '/') {
		int ret_val = handle_command_message(message, client);

		if (ret_val == CLIENT_EXIT) {
			return SUCCESS;
		}
	}
	else {
		std::string chat_message_index = std::to_string(static_cast<int>(server->chat_messages.size()));
		std::string chat_message  = chat_message_index + " " + client.name + ": " + message;

		server->chat_messages.push_back(chat_message);
		send_message_to_all(chat_message);

		server->message_count++;
	}

	log("Message from " + client.ip + ":" + client.port + " - " + message);

	return SUCCESS;
}

int handle_client(CLIENT& client) {
	setlocale(LC_ALL, "rus");

	int ret_val;
	std::string message;

	while (true) {
		message.clear();
		message.resize(1024);

		log("Receiving data from client " + client.ip + ":" + client.port);
		
		ret_val = recv(client.socket, (char*)message.data(), message.size(), 0);

		std::lock_guard receiving_message_mutex_lock(*server->receiving_message_mutex);

		if (ret_val <= 0) {
			handle_command_message("/exit", client);
			return CLIENT_EXIT;
		}

		message.resize(ret_val);

		handle_message(message, client);
	}
}

void start_handle_client(const CLIENT& client) {
	auto [pair_socket_client, emplaced] = server->clients.emplace(client.socket, std::move(client));

	std::thread handle_client_thread(handle_client, std::ref(pair_socket_client->second));

	handle_client_thread.detach();
}

std::optional<CLIENT> accept_client() {
	log("Accepting the client socket");

	int client_socket;
	sockaddr_in client_sockaddr{};

	socklen_t client_addr_size = sizeof(client_sockaddr);

	client_socket = accept(server->socket, (sockaddr*)&client_sockaddr, &client_addr_size);

	if (client_socket <= 0) {
		perror("accept");
		close(server->socket);
		return std::nullopt;
	}

	char client_ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &client_sockaddr.sin_addr, client_ip, INET_ADDRSTRLEN);

	std::string client_port = std::to_string(ntohs(client_sockaddr.sin_port));
	CLIENT client(client_socket, client_ip, client_port);

	return client;
}

int listen_server_socket() {

	int clients_count = 15;
	int ret_val;

	log("Listening to the server socket");

	ret_val = listen(server->socket, clients_count);

	if (ret_val < 0) {
		perror("listen");
		close(server->socket);
		return ERR;
	}

	return SUCCESS;
}

int get_server_ips() {
	std::cout << "Server IPs: \n\n";

	ifaddrs* interface_addrs = nullptr;

	if (getifaddrs(&interface_addrs) == -1) {
		perror("getifaddrs");
		return -1;
	}

	for (ifaddrs* interface_addr = interface_addrs; interface_addr != nullptr;
		interface_addr = interface_addr->ifa_next) {
		if (interface_addr->ifa_addr == nullptr) continue;

		if (interface_addr->ifa_addr->sa_family == AF_INET) { // IPv4
			void* addr = &((sockaddr_in*)interface_addr->ifa_addr)->sin_addr;
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, addr, ip, sizeof(ip));

			std::cout << interface_addr->ifa_name << " - " << ip << std::endl;
		}
	}

	std::string external_ip = "217.71.129.139";
	std::cout << "extetrnal: " << external_ip;

	freeifaddrs(interface_addrs);

	std::cout << "\n\n";

	return 0;
}

std::optional<SERVER> get_server() {
	std::cout << "Port: ";
	int port = 0;
	std::cin >> port;
	std::cout << "\n\n";

	int ret_val;

	SERVER server;

	server.sockaddr.sin_family = AF_INET;
	server.sockaddr.sin_addr.s_addr = INADDR_ANY;
	server.sockaddr.sin_port = htons(port);

	log("Creating the server socket");

	server.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server.socket <= 0) {
		perror("socket");
		return std::nullopt;
	}

	log("Binding the server socket");

	int option = 1;
	setsockopt(server.socket, SOL_SOCKET, SO_REUSEADDR, (char*)&option, sizeof(option));

	ret_val = bind(server.socket, (sockaddr*)&server.sockaddr, sizeof(server.sockaddr));

	if (ret_val < 0) {
		perror("bind");
		close(server.socket);
		return std::nullopt;
	}

	return server;

}

void start_logging() {
	std::thread logging_thread(logging);
	logging_thread.detach();
}

int main() {
	setlocale(LC_ALL, "rus");

	start_logging();

	get_server_ips();

	server = get_server();

	if (!server)
		return ERR;

	while (true) {
		if (listen_server_socket() == ERR)
			return ERR;
		
		auto client = accept_client();
		if (client.has_value())
			start_handle_client(*client);
	}

}