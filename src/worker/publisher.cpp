#include "publisher.hpp"

#include <obelisk/zmq_message.hpp>
#include "echo.hpp"

#define LOG_PUBLISHER LOG_WORKER

using namespace bc;
using std::placeholders::_1;
using std::placeholders::_2;

publisher::publisher(node_impl& node)
  : node_(node), context_(1)
{
    node.subscribe_blocks(std::bind(&publisher::send_blk, this, _1, _2));
    node.subscribe_transactions(std::bind(&publisher::send_tx, this, _1));
}

bool publisher::setup_socket(const std::string& connection,
    zmq_socket_uniqptr& socket)
{
    if (connection != "")
    {
        socket.reset(new zmq::socket_t(context_, ZMQ_PUB));
        socket->bind(connection.c_str());
        return true;
    }
    return false;
}

bool publisher::start(config_map_type& config)
{
    if (setup_socket(config["block-publish"], socket_block_))
        log_debug(LOG_PUBLISHER) << "Publishing blocks: "
            << config["block-publish"];
    if (setup_socket(config["tx-publish"], socket_tx_))
        log_debug(LOG_PUBLISHER) << "Publishing transactions: "
            << config["tx-publish"];
    return true;
}

bool publisher::stop()
{
    return true;
}

bool send_raw(const bc::data_chunk& raw,
    zmq::socket_t& socket, bool send_more=false)
{
    zmq::message_t message(raw.size());
    memcpy(message.data(), raw.data(), raw.size());
    return socket.send(message, send_more ? ZMQ_SNDMORE : 0);
}

void append_hash(zmq_message& message, const hash_digest& hash)
{
    message.append(data_chunk(hash.begin(), hash.end()));
}

bool publisher::send_blk(uint32_t height, const block_type& blk)
{
    data_chunk raw_height = bc::uncast_type(height);
    BITCOIN_ASSERT(raw_height.size() == 4);
    data_chunk raw_block(bc::satoshi_raw_size(blk));
    satoshi_save(blk, raw_block.begin());
    zmq_message message;
    message.append(raw_height);
    append_hash(message, hash_block_header(blk.header));
    message.append(raw_block);
    if (!message.send(*socket_block_))
    {
        log_warning(LOG_PUBLISHER) << "Problem publishing block data.";
        return false;
    }
    return true;
}

bool publisher::send_tx(const transaction_type& tx)
{
    data_chunk raw_tx(bc::satoshi_raw_size(tx));
    satoshi_save(tx, raw_tx.begin());
    zmq_message message;
    append_hash(message, hash_transaction(tx));
    message.append(raw_tx);
    if (!message.send(*socket_tx_))
    {
        log_warning(LOG_PUBLISHER) << "Problem publishing tx data.";
        return false;
    }
    return true;
}
