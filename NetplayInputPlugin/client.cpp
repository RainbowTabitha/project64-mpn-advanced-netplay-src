#include "stdafx.h"

#include "client.h"
#include "client_dialog.h"
#include "util.h"

using namespace std;
using namespace asio;

client::client(shared_ptr<io_service> io_s, shared_ptr<client_dialog> my_dialog)
    : connection(io_s), my_dialog(my_dialog), work(*io_s), resolver(*io_s), thread([&] { io_s->run(); }) {

    my_dialog->set_message_handler([=](string message) {
        this->io_s->post([=] { process_message(message); });
    });

    my_dialog->set_close_handler([=] {
        this->io_s->post([=] {
            if (started) {
                my_dialog->minimize();
            } else {
                my_dialog->destroy();
                close();
                map_local_to_netplay();
                start_game();
            }
        });
    });

    lag = DEFAULT_LAG;
    frame = 0;
    golf = false;

    my_dialog->status("List of available commands:\n"
                      "- /name <name>            Set your name\n"
                      "- /host [port]            Host a server\n"
                      "- /join <address> [port]  Join a server\n"
                      "- /start                  Start the game\n"
                      "- /lag <lag>              Set the netplay input lag\n"
                      "- /autolag                Toggle automatic lag on and off\n"
                      "- /golf                   Toggle golf mode on and off");
}

client::~client() {
    if (thread.get_id() != this_thread::get_id()) {
        io_s->stop();
        thread.join();
    } else {
        thread.detach();
    }
}

string client::get_name() {
    promise<string> promise;
    io_s->post([&] { promise.set_value(name); });
    return promise.get_future().get();
}

void client::set_name(const string& name) {
    promise<void> promise;
    io_s->post([&] {
        this->name = name;
        my_dialog->status("Your name is " + name);
        promise.set_value();
    });
    promise.get_future().get();
}

void client::set_local_controllers(CONTROL controllers[MAX_PLAYERS]) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        controllers[i].RawData = FALSE; // Disallow raw data
    }

    promise<void> promise;
    io_s->post([&] {
        for (size_t i = 0; i < MAX_PLAYERS; i++) {
            local_controllers[i] = controllers[i];
        }
        send_controllers();
        promise.set_value();
    });
    promise.get_future().get();
}

void client::process_input(BUTTONS local_input[MAX_PLAYERS]) {
    promise<void> promise;
    io_s->post([&] {
        for (int netplay_port = 0; netplay_port < MAX_PLAYERS; netplay_port++) {
            int local_port = my_controller_map.to_local(netplay_port);
            if (local_port >= 0) {
                if (golf && lag != 0 && local_input[local_port].Z_TRIG) {
                    send_lag(lag);
                    set_lag(0);
                }
                while (input_queues[netplay_port].size() <= lag) {
                    input_queues[netplay_port].push(local_input[local_port]);
                    send_input(netplay_port, local_input[local_port]);
                }
            } else if (netplay_controllers[netplay_port].Present && !socket.is_open()) {
                while (input_queues[netplay_port].size() <= lag) {
                    input_queues[netplay_port].push(BUTTONS{ 0 });
                }
            }
        }

        send_frame();
        frame++;

        promise.set_value();
    });
    promise.get_future().get();
}

void client::get_input(int port, BUTTONS* input) {
    if (netplay_controllers[port].Present) {
        *input = input_queues[port].pop();
    } else {
        input->Value = 0;
    }
}

void client::set_netplay_controllers(CONTROL netplay_controllers[MAX_PLAYERS]) {
    promise<void> promise;
    io_s->post([&] {
        this->netplay_controllers = netplay_controllers;
        promise.set_value();
    });
    promise.get_future().get();
}

void client::post_close() {
    io_s->post([&] {
        close();
        map_local_to_netplay();
        start_game();
    });
}

void client::wait_until_start() {
    if (started) return;

    unique_lock<mutex> lock(mut);
    start_condition.wait(lock, [=] { return started; });
}

void client::process_message(string message) {
    if (message.substr(0, 1) == "/") {
        vector<string> params;
        for (int start = 0, end = 0; end != string::npos; start = end + 1) {
            end = message.find(" ", start);
            string param = message.substr(start, end == string::npos ? string::npos : end - start);
            if (!param.empty()) params.push_back(param);
        }

        if (params[0] == "/name") {
            if (params.size() >= 2) {
                name = params[1];
                my_dialog->status("Your name is now " + name);
                send_name();
            } else {
                my_dialog->error("Missing parameter");
            }
        } else if (params[0] == "/host" || params[0] == "/server") {
            if (started) {
                my_dialog->error("Game has already started");
                return;
            }

            try {
                uint16_t port = params.size() >= 2 ? stoi(params[1]) : 6400;

                close();
                my_server = make_shared<server>(io_s, lag);
                port = my_server->open(port);

                my_dialog->status("Server is listening on port " + to_string(port) + "...");

                if (port) {
                    connect("127.0.0.1", port);
                }
            } catch(const exception& e) {
                my_dialog->error(e.what());
            }
        } else if (params[0] == "/join" || params[0] == "/connect") {
            if (started) {
                my_dialog->error("Game has already started");
                return;
            }

            if (params.size() < 2) {
                my_dialog->error("Missing parameter");
                return;
            }

            string host = params[1];

            try {
                uint16_t port = params.size() >= 3 ? stoi(params[2]) : 6400;
                close();
                connect(host, port);
            } catch (const exception& e) {
                my_dialog->error(e.what());
            }
        } else if (params[0] == "/start") {
            if (started) {
                my_dialog->error("Game has already started");
                return;
            }
            if (socket.is_open()) {
                send_start_game();
            } else {
                map_local_to_netplay();
                set_lag(0);
                start_game();
            }
        } else if (params[0] == "/lag") {
            if (params.size() >= 2) {
                try {
                    uint8_t lag = stoi(params[1]);
                    set_lag(lag);
                    send_lag(lag);
                } catch(const exception& e) {
                    my_dialog->error(e.what());
                }
            } else {
                my_dialog->error("Missing parameter");
            }
        } else if (params[0] == "/autolag") {
            send_autolag();
        } else if (params[0] == "/my_lag") {
            if (params.size() >= 2) {
                try {
                    uint8_t lag = stoi(params[1]);
                    set_lag(lag);
                } catch (const exception& e) {
                    my_dialog->error(e.what());
                }
            } else {
                my_dialog->error("Missing parameter");
            }
        } else if (params[0] == "/your_lag") {
            if (params.size() >= 2) {
                try {
                    uint8_t lag = stoi(params[1]);
                    send_lag(lag);
                } catch (const exception& e) {
                    my_dialog->error(e.what());
                }
            } else {
                my_dialog->error("Missing parameter");
            }
        } else if (params[0] == "/golf") {
            golf = !golf;
            
            if (golf) {
                my_dialog->status("Golf mode is enabled");
            } else {
                my_dialog->status("Golf mode is disabled");
            }
        } else {
            my_dialog->error("Unknown command: " + params[0]);
        }
    } else {
        my_dialog->chat(name, message);
        send_chat(message);
    }
}

void client::set_lag(uint8_t lag, bool show_message) {
    this->lag = lag;

    if (show_message) {
        my_dialog->status("Your lag is set to " + to_string(lag));
    }
}

void client::remove_user(uint32_t user_id) {
    my_dialog->status(users[user_id].name + " has quit");
    users.erase(user_id);
    my_dialog->update_user_list(users);
}

void client::chat_received(int32_t user_id, const string& message) {
    switch (user_id) {
        case -2:
            my_dialog->error(message);
            break;

        case -1:
            my_dialog->status(message);
            break;

        default:
            my_dialog->chat(users[user_id].name, message);
    }
}

uint8_t client::get_total_count() {
    uint8_t count = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (netplay_controllers[i].Present) {
            count++;
        }
    }

    return count;
}

void client::close() {
    resolver.cancel();

    error_code error;
    socket.shutdown(ip::tcp::socket::shutdown_both, error);
    socket.close(error);

    if (my_server) {
        my_server->close();
        my_server.reset();
    }

    users.clear();
    my_dialog->update_user_list(users);
}

void client::start_game() {
    unique_lock<mutex> lock(mut);
    if (started) return;

    started = true;
    start_condition.notify_all();

    my_dialog->status("Starting game...");
}

void client::handle_error(const error_code& error) {
    if (error == error::operation_aborted) return;

    close();

    for (int i = 0; i < MAX_PLAYERS; i++) {
        input_queues[i].push(BUTTONS{ 0 }); // Dummy input to unblock queue
    }

    my_dialog->error(error == error::eof ? "Disconnected from server" : error.message());
}

void client::connect(const string& host, uint16_t port) {
    my_dialog->status("Connecting to " + host + ":" + to_string(port) + "...");
    resolver.async_resolve(ip::tcp::resolver::query(host, to_string(port)), [=](const error_code& error, ip::tcp::resolver::iterator iterator) {
        if (error) return my_dialog->error(error.message());
        ip::tcp::endpoint endpoint = *iterator;
        socket.async_connect(endpoint, [=](const error_code& error) {
            if (error) return my_dialog->error(error.message());

            error_code ec;
            socket.set_option(ip::tcp::no_delay(true), ec);
            if (ec) return my_dialog->error(ec.message());

            my_dialog->status("Connected!");

            send_join();

            process_packet();
        });
    });
}

void client::process_packet() {
    auto self(shared_from_this());
    read([=](packet& p) {
        if (p.size() == 0) return self->process_packet();

        auto packet_type = p.read<uint8_t>();
        switch (packet_type) {
            case VERSION: {
                auto protocol_version = p.read<uint32_t>();
                if (protocol_version != PROTOCOL_VERSION) {
                    close();
                    my_dialog->error("Server protocol version does not match client protocol version");
                }
                break;
            }

            case JOIN: {
                auto user_id = p.read<uint32_t>();
                auto name_length = p.read<uint8_t>();
                string name(name_length, ' ');
                p.read(name);
                my_dialog->status(name + " has joined");
                users[user_id].name = name;
                my_dialog->update_user_list(users);
                break;
            }

            case PING: {
                auto timestamp = p.read<uint64_t>();
                send(packet() << PONG << timestamp);
                break;
            }

            case LATENCY: {
                while (p.bytes_remaining() >= 8) {
                    auto user_id = p.read<uint32_t>();
                    auto latency = p.read<uint32_t>();
                    users[user_id].latency = latency;
                }
                my_dialog->update_user_list(users);
                break;
            }

            case NAME: {
                auto user_id = p.read<uint32_t>();
                auto name_length = p.read<uint8_t>();
                string name(name_length, ' ');
                p.read(name);
                my_dialog->status(users[user_id].name + " is now " + name);
                users[user_id].name = name;
                my_dialog->update_user_list(users);
                break;
            }

            case QUIT: {
                auto user_id = p.read<uint32_t>();
                remove_user(user_id);
                break;
            }

            case MESSAGE: {
                auto user_id = p.read<int32_t>();
                auto message_length = p.read<uint16_t>();
                string message(message_length, ' ');
                p.read(message);
                chat_received(user_id, message);
                break;
            }

            case CONTROLLERS: {
                auto user_id = p.read<int32_t>();
                if (user_id == -1) {
                    for (size_t i = 0; i < MAX_PLAYERS; i++) {
                        p >> netplay_controllers[i].Plugin;
                        p >> netplay_controllers[i].Present;
                        p >> netplay_controllers[i].RawData;
                    }
                    for (size_t i = 0; i < MAX_PLAYERS; i++) {
                        my_controller_map.local_to_netplay[i] = p.read<int8_t>();
                    }
                } else {
                    for (size_t i = 0; i < MAX_PLAYERS; i++) {
                        p >> users[user_id].controllers[i].plugin;
                        p >> users[user_id].controllers[i].present;
                        p >> users[user_id].controllers[i].raw_data;
                    }
                    for (size_t i = 0; i < MAX_PLAYERS; i++) {
                        users[user_id].control_map.local_to_netplay[i] = p.read<int8_t>();
                    }
                    my_dialog->update_user_list(users);
                }
                break;
            }

            case START: {
                start_game();
                break;
            }

            case INPUT_DATA: {
                auto port = p.read<uint8_t>();
                BUTTONS buttons;
                buttons.Value = p.read<uint32_t>();
                try {
                    input_queues[port].push(buttons);
                } catch (const exception&) {}
                break;
            }

            case LAG: {
                auto lag = p.read<uint8_t>();
                set_lag(lag, false);
                break;
            }
        }

        self->process_packet();
    });
}

void client::map_local_to_netplay() {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        netplay_controllers[i] = local_controllers[i];
        if (local_controllers[i].Present) {
            my_controller_map.insert(i, i);
        }
    }
}

void client::send_join() {
    if (!socket.is_open()) return;

    packet p;

    p << JOIN << PROTOCOL_VERSION << (uint8_t)name.size() << name;

    for (auto& c : local_controllers) {
        p << c.Plugin << c.Present << c.RawData;
    }

    send(p);
}

void client::send_name() {
    if (!socket.is_open()) return;

    packet p;
    p << NAME;
    p << (uint8_t)name.size();
    p << name;

    send(p);
}

void client::send_chat(const string& message) {
    if (!socket.is_open()) return;

    send(packet() << MESSAGE << (uint16_t)message.size() << message);
}

void client::send_controllers() {
    if (!socket.is_open()) return;

    packet p;
    p << CONTROLLERS;
    for (auto& c : local_controllers) {
        p << c.Plugin << c.Present << c.RawData;
    }
    send(p);
}

void client::send_start_game() {
    send(packet() << START << 0);
}

void client::send_lag(uint8_t lag) {
    if (!socket.is_open()) return;

    send(packet() << LAG << lag);
}

void client::send_autolag() {
    if (!socket.is_open()) return my_dialog->error("Cannot toggle automatic lag unless connected to server");

    send(packet() << AUTOLAG);
}

void client::send_input(uint8_t port, BUTTONS input) {
    if (!socket.is_open()) return;

    send(packet() << INPUT_DATA << port << input.Value, false);
}

void client::send_frame() {
    if (!socket.is_open()) return;

    send(packet() << FRAME << frame);
}
