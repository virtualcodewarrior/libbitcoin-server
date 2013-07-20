#ifndef OBELISK_WORKER_WORKER
#define OBELISK_WORKER_WORKER

#include <unordered_map>
#include <zmq.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <shared/message.hpp>

typedef std::shared_ptr<zmq::socket_t> socket_ptr;

bool send_string(zmq::socket_t& socket, const std::string& str);

class request_worker
{
public:
    typedef std::function<void (
        const incoming_message&, socket_ptr)> command_handler;

    request_worker();
    void attach(const std::string& command, command_handler handler);
    void update();

private:
    typedef std::unordered_map<std::string, command_handler> command_map;

    void create_new_socket();

    zmq::context_t context_;
    socket_ptr socket_;

    boost::posix_time::ptime last_heartbeat_;
    // Send out heartbeats at regular intervals
    boost::posix_time::ptime heartbeat_at_;
    size_t interval_;

    command_map handlers_;
};

#endif

