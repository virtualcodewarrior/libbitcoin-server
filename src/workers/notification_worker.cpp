/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-server.
 *
 * libbitcoin-server is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/server/workers/notification_worker.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <bitcoin/protocol.hpp>
#include <bitcoin/server/messages/message.hpp>
#include <bitcoin/server/messages/route.hpp>
#include <bitcoin/server/server_node.hpp>
#include <bitcoin/server/services/query_service.hpp>
#include <bitcoin/server/settings.hpp>
#include <bitcoin/server/utility/fetch_helpers.hpp>

namespace libbitcoin {
namespace server {

#define NAME "notification_worker"

using namespace std::placeholders;
using namespace bc::chain;
using namespace bc::protocol;
using namespace bc::wallet;

static const std::string address_update = "address.update";
static const std::string address_update2 = "address.update2";
static const std::string address_stealth = "address.stealth_update";

notification_worker::notification_worker(zmq::authenticator& authenticator,
    server_node& node, bool secure)
  : worker(node.thread_pool()),
    secure_(secure),
    settings_(node.server_settings()),
    node_(node),
    authenticator_(authenticator),
    payment_subscriber_(std::make_shared<payment_subscriber>(
        node.thread_pool(), NAME "_payment")),
    stealth_subscriber_(std::make_shared<stealth_subscriber>(
        node.thread_pool(), NAME "_stealth")),
    address_subscriber_(std::make_shared<address_subscriber>(
        node.thread_pool(), NAME "_address")),
    penetration_subscriber_(std::make_shared<penetration_subscriber>(
        node.thread_pool(), NAME "_penetration"))
{
}

// There is no unsubscribe so this class shouldn't be restarted.
bool notification_worker::start()
{
    // v2/v3 (deprecated)
    payment_subscriber_->start();
    stealth_subscriber_->start();

    // v3
    address_subscriber_->start();
    penetration_subscriber_->start();

    // Subscribe to blockchain reorganizations.
    node_.subscribe_blockchain(
        std::bind(&notification_worker::handle_blockchain_reorganization,
            this, _1, _2, _3, _4));

    // Subscribe to transaction pool acceptances.
    node_.subscribe_transaction_pool(
        std::bind(&notification_worker::handle_transaction_pool,
            this, _1, _2, _3));

    // Subscribe to all inventory messages from all peers.
    node_.subscribe<bc::message::inventory>(
        std::bind(&notification_worker::handle_inventory,
            this, _1, _2));

    return zmq::worker::start();
}

// No unsubscribe so must be kept in scope until subscriber stop complete.
bool notification_worker::stop()
{
    static const auto code = error::channel_stopped;

    // v2/v3 (deprecated)
    payment_subscriber_->stop();
    payment_subscriber_->do_relay(code, {}, 0, {}, {});

    stealth_subscriber_->stop();
    stealth_subscriber_->do_relay(code, 0, 0, {}, {});

    // v3
    address_subscriber_->stop();
    address_subscriber_->do_relay(code, {}, 0, {}, {});

    penetration_subscriber_->stop();
    penetration_subscriber_->do_relay(code, 0, {}, {});

    return zmq::worker::stop();
}

// Implement worker as a router to the query service.
// The notification worker receives no messages from the query service.
void notification_worker::work()
{
    zmq::socket router(authenticator_, zmq::socket::role::router);

    // Connect socket to the service endpoint.
    if (!started(connect(router)))
        return;

    zmq::poller poller;
    poller.add(router);

    // We do not send/receive on the poller, we use its timer and context stop.
    // Other threads connect and disconnect dynamically to send updates.
    while (!poller.terminated() && !stopped())
    {
        poller.wait();
        prune();
    }

    // Disconnect the socket and exit this thread.
    finished(disconnect(router));
}

// Connect/Disconnect.
//-----------------------------------------------------------------------------

bool notification_worker::connect(socket& router)
{
    const auto security = secure_ ? "secure" : "public";
    const auto& endpoint = secure_ ? query_service::secure_notify :
        query_service::public_notify;

    const auto ec = router.connect(endpoint);

    if (ec)
    {
        log::error(LOG_SERVER)
            << "Failed to connect " << security << " notification worker to "
            << endpoint << " : " << ec.message();
        return false;
    }

    log::debug(LOG_SERVER)
        << "Connected " << security << " notification worker to " << endpoint;
    return true;
}

bool notification_worker::disconnect(socket& router)
{
    const auto security = secure_ ? "secure" : "public";

    // Don't log stop success.
    if (router.stop())
        return true;

    log::error(LOG_SERVER)
        << "Failed to disconnect " << security << " notification worker.";
    return false;
}

// Pruning.
// ----------------------------------------------------------------------------

// Signal expired subscriptions to self-remove.
void notification_worker::prune()
{
    static const auto code = error::channel_timeout;

    // v2/v3 (deprecated)
    payment_subscriber_->relay(code, {}, 0, {}, {});
    stealth_subscriber_->relay(code, 0, 0, {}, {});

    // v3
    address_subscriber_->relay(code, {}, 0, {}, {});
    penetration_subscriber_->relay(code, 0, {}, {});
}

// Sending.
// ----------------------------------------------------------------------------

//*****************************************************************************
// TODO: the notify_dealer must be connected to the query_service router.
//*****************************************************************************
void notification_worker::send(const route& reply_to,
    const std::string& command, uint32_t id, const data_chunk& payload)
{
    const auto security = secure_ ? "secure" : "public";
    const auto& endpoint = secure_ ? query_service::secure_notify :
        query_service::public_notify;

    zmq::socket notifier(authenticator_, zmq::socket::role::router);
    auto ec = notifier.connect(endpoint);

    if (ec == bc::error::service_stopped)
        return;

    if (ec)
    {
        log::warning(LOG_SERVER)
            << "Failed to connect " << security << " notification worker: "
            << ec.message();
        return;
    }

    // Notifications are formatted as query response messages.
    message notification(reply_to, command, id, payload);
    ec = notification.send(notifier);

    if (ec && ec != error::service_stopped)
        log::warning(LOG_SERVER)
            << "Failed to send notification to "
            << notification.route().display() << " " << ec.message();
}

void notification_worker::send_payment(const route& reply_to, uint32_t id,
    const wallet::payment_address& address, uint32_t height,
    const hash_digest& block_hash, const chain::transaction& tx)
{
    // [ address.version:1 ]
    // [ address.hash:20 ]
    // [ height:4 ]
    // [ block_hash:32 ]
    // [ tx:... ]
    const auto payload = build_chunk(
    {
        to_array(address.version()),
        address.hash(),
        to_little_endian(height),
        block_hash,
        tx.to_data()
    });
    
    send(reply_to, address_update, id, payload);
}

void notification_worker::send_stealth(const route& reply_to, uint32_t id,
    uint32_t prefix, uint32_t height, const hash_digest& block_hash,
    const chain::transaction& tx)
{
    // [ prefix:4 ]
    // [ height:4 ]
    // [ block_hash:32 ]
    // [ tx:... ]
    const auto payload = build_chunk(
    {
        to_little_endian(prefix),
        to_little_endian(height),
        block_hash,
        tx.to_data()
    });

    send(reply_to, address_stealth, id, payload);
}

void notification_worker::send_address(const route& reply_to, uint32_t id,
    uint8_t sequence, uint32_t height, const hash_digest& block_hash,
    const chain::transaction& tx)
{
    // [ code:4 ]
    // [ sequence:1 ]
    // [ height:4 ]
    // [ block_hash:32 ]
    // [ tx:... ]
    const auto payload = build_chunk(
    {
        message::to_bytes(error::success),
        to_array(sequence),
        to_little_endian(height),
        block_hash,
        tx.to_data()
    });

    send(reply_to, address_update2, id, payload);
}

// Handlers.
// ----------------------------------------------------------------------------

bool notification_worker::handle_payment(const code& ec,
    const payment_address& address, uint32_t height,
    const hash_digest& block_hash, const chain::transaction& tx,
    const route& reply_to, uint32_t id, const binary& prefix_filter)
{
    if (ec == error::channel_timeout && true /* not expired */)
    {
        return true;
    }

    if (ec)
    {
        send(reply_to, address_update, id, message::to_bytes(ec));
        return false;
    }

    if (prefix_filter.is_prefix_of(address.hash()))
        send_payment(reply_to, id, address, height, block_hash, tx);

    return true;
}

bool notification_worker::handle_stealth(const code& ec,
    uint32_t prefix, uint32_t height, const hash_digest& block_hash,
    const chain::transaction& tx, const route& reply_to, uint32_t id,
    const binary& prefix_filter)
{
    if (ec == error::channel_timeout && true /* not expired */)
    {
        return true;
    }

    if (ec)
    {
        send(reply_to, address_stealth, id, message::to_bytes(ec));
        return false;
    }

    if (prefix_filter.is_prefix_of(prefix))
        send_stealth(reply_to, id, prefix, height, block_hash, tx);

    return true;
}

bool notification_worker::handle_address(const code& ec,
    const binary& field, uint32_t height, const hash_digest& block_hash,
    const chain::transaction& tx, const route& reply_to, uint32_t id,
    const binary& prefix_filter, std::shared_ptr<uint8_t> sequence)
{
    if (ec == error::channel_timeout && true /* not expired */)
    {
        return true;
    }

    if (ec)
    {
        send(reply_to, address_update2, id, message::to_bytes(ec));
        return false;
    }

    if (prefix_filter.is_prefix_of(field))
        send_address(reply_to, id, *sequence, height, block_hash, tx);

    return true;
}

// Subscribers.
// ----------------------------------------------------------------------------

// Subscribe to address and stealth prefix notifications.
// Each delegate must connect to the appropriate query notification endpoint.
void notification_worker::subscribe_address(const route& reply_to,
    const binary& prefix_filter, subscribe_type type)
{
    static const auto code = error::channel_stopped;

    switch (type)
    {
        // v2/v3 (deprecated)
        case subscribe_type::payment:
        {
            const auto handler =
                std::bind(&notification_worker::handle_payment,
                    this, _1, _2, _3, _4, _5, reply_to, 0, prefix_filter);

            payment_subscriber_->subscribe(handler, code, {}, 0, {}, {});
            break;
        }

        // v2/v3 (deprecated)
        case subscribe_type::stealth:
        {
            const auto handler =
                std::bind(&notification_worker::handle_stealth,
                    this, _1, _2, _3, _4, _5, reply_to, 0, prefix_filter);

            stealth_subscriber_->subscribe(handler, code, 0, 0, {}, {});
            break;
        }

        // v3
        default:
        case subscribe_type::unspecified:
        {
            // The sequence enables the client to detect dropped messages.
            const auto sequence = std::make_shared<uint8_t>(0);

            const auto handler =
                std::bind(&notification_worker::handle_address,
                    this, _1, _2, _3, _4, _5, reply_to, 0, prefix_filter,
                    sequence);

            // v3
            address_subscriber_->subscribe(handler, code, {}, 0, {}, {});
            break;
        }
    }
}

// Subscribe to transaction penetration notifications.
// Each delegate must connect to the appropriate query notification endpoint.
void notification_worker::subscribe_penetration(const route& reply_to,
    const hash_digest& tx_hash)
{
    // TODO:
    // Height and hash are zeroized if tx is not chained (inv/mempool).
    // If chained or penetration is 100 (percent) drop subscription.
    // Only send messages at configured thresholds (e.g. 20/40/60/80/100%).
    // Thresholding allows the server to mask its peer count.
    // Penetration is computed by the relay handler.
    // No sequence is required because gaps are okay.
    // [ tx_hash:32 ]
    // [ penetration:1 ]
    // [ height:4 ]
    // [ block_hash:32 ]
    ////penetration_subscriber_->subscribe();
}

// Notification (via blockchain).
// ----------------------------------------------------------------------------

bool notification_worker::handle_blockchain_reorganization(const code& ec,
    uint64_t fork_point, const block_list& new_blocks, const block_list&)
{
    if (stopped() || ec == bc::error::service_stopped)
        return false;

    if (ec)
    {
        log::warning(LOG_SERVER)
            << "Failure handling new block: " << ec.message();

        // Don't let a failure here prevent prevent future notifications.
        return true;
    }

    // Blockchain height is 64 bit but obelisk protocol is 32 bit.
    BITCOIN_ASSERT(fork_point <= max_uint32);
    const auto fork_point32 = static_cast<uint32_t>(fork_point);

    notify_blocks(fork_point32, new_blocks);
    return true;
}

void notification_worker::notify_blocks(uint32_t fork_point,
    const block_list& blocks)
{
    if (stopped())
        return;

    const auto security = secure_ ? "secure" : "public";
    const auto& endpoint = secure_ ? block_service::secure_worker :
        block_service::public_worker;

    // Notifications are off the pub-sub thread so this must connect back.
    // This could be optimized by caching the socket as thread static.
    zmq::socket publisher(authenticator_, zmq::socket::role::publisher);
    const auto ec = publisher.connect(endpoint);

    if (ec == bc::error::service_stopped)
        return;

    if (ec)
    {
        log::warning(LOG_SERVER)
            << "Failed to connect " << security << " block worker: "
            << ec.message();
        return;
    }

    BITCOIN_ASSERT(blocks.size() <= max_uint32);
    BITCOIN_ASSERT(fork_point < max_uint32 - blocks.size());
    auto height = fork_point;

    for (const auto block: blocks)
        notify_block(publisher, height++, block);
}

void notification_worker::notify_block(zmq::socket& publisher, uint32_t height,
    const block::ptr block)
{
    if (stopped())
        return;

    const auto block_hash = block->header.hash();

    for (const auto& tx: block->transactions)
    {
        const auto tx_hash = tx.hash();

        notify_transaction(height, block_hash, tx);
        notify_penetration(height, block_hash, tx_hash);
    }
}

// Notification (via transaction inventory).
// ----------------------------------------------------------------------------
// This relies on peers always notifying us of new txs via inv messages.

bool notification_worker::handle_inventory(const code& ec,
    const bc::message::inventory::ptr packet)
{
    if (stopped() || ec == bc::error::service_stopped)
        return false;

    if (ec)
    {
        log::warning(LOG_SERVER)
            << "Failure handling inventory: " << ec.message();

        // Don't let a failure here prevent prevent future notifications.
        return true;
    }

    // Loop inventories and extract transaction hashes.
    for (const auto& inventory: packet->inventories)
        if (inventory.type == bc::message::inventory_type_id::transaction)
            notify_penetration(0, null_hash, inventory.hash);

    return true;
}

// Notification (via mempool).
// ----------------------------------------------------------------------------

bool notification_worker::handle_transaction_pool(const code& ec,
    const point::indexes&, const transaction& tx)
{
    if (stopped() || ec == bc::error::service_stopped)
        return false;

    if (ec)
    {
        log::warning(LOG_SERVER)
            << "Failure handling new transaction: " << ec.message();

        // Don't let a failure here prevent future notifications.
        return true;
    }

    notify_transaction(0, null_hash, tx);
    return true;
}

// This parsing is duplicated by bc::database::data_base.
void notification_worker::notify_transaction(uint32_t height,
    const hash_digest& block_hash, const transaction& tx)
{
    uint32_t prefix;

    // TODO: move full integer and array constructors into binary.
    static constexpr size_t prefix_bits = sizeof(prefix) * byte_bits;
    static constexpr size_t address_bits = short_hash_size * byte_bits;

    if (stopped() || tx.outputs.empty())
        return;

    // see data_base::push_inputs
    // Loop inputs and extract payment addresses.
    for (const auto& input: tx.inputs)
    {
        const auto address = payment_address::extract(input.script);

        if (address)
        {
            const binary field(address_bits, address.hash());
            notify_address(field, height, block_hash, tx);
            notify_payment(address, height, block_hash, tx);
        }
    }

    // see data_base::push_outputs
    // Loop outputs and extract payment addresses.
    for (const auto& output: tx.outputs)
    {
        const auto address = payment_address::extract(output.script);

        if (address)
        {
            const binary field(address_bits, address.hash());
            notify_address(field, height, block_hash, tx);
            notify_payment(address, height, block_hash, tx);
        }
    }

    // see data_base::push_stealth
    // Loop output pairs and extract stealth payments.
    for (size_t index = 0; index < (tx.outputs.size() - 1); ++index)
    {
        const auto& ephemeral_script = tx.outputs[index].script;
        const auto& payment_script = tx.outputs[index + 1].script;

        // Try to extract a stealth prefix from the first output.
        // Try to extract the payment address from the second output.
        if (to_stealth_prefix(prefix, ephemeral_script) &&
            payment_address::extract(payment_script))
        {
            const binary field(prefix_bits, to_little_endian(prefix));
            notify_address(field, height, block_hash, tx);
            notify_stealth(prefix, height, block_hash, tx);
        }
    }
}

// v2/v3 (deprecated)
void notification_worker::notify_payment(const payment_address& address,
    uint32_t height, const hash_digest& block_hash, const transaction& tx)
{
    static const auto code = error::success;
    payment_subscriber_->relay(code, address, height, block_hash, tx);
}

// v2/v3 (deprecated)
void notification_worker::notify_stealth(uint32_t prefix, uint32_t height,
    const hash_digest& block_hash, const transaction& tx)
{
    static const auto code = error::success;
    stealth_subscriber_->relay(code, prefix, height, block_hash, tx);
}

// v3
void notification_worker::notify_address(const binary& field, uint32_t height,
    const hash_digest& block_hash, const transaction& tx)
{
    static const auto code = error::success;
    address_subscriber_->relay(code, field, height, block_hash, tx);
}

// v3.x
void notification_worker::notify_penetration(uint32_t height,
    const hash_digest& block_hash, const hash_digest& tx_hash)
{
    static const auto code = error::success;
    penetration_subscriber_->relay(code, height, block_hash, tx_hash);
}

// reference
// ----------------------------------------------------------------------------

////void notification_worker::subscribe(const incoming& request, send_handler handler)
////{
////    const auto ec = create(request, handler);
////
////    // Send response.
////    handler(outgoing(request, ec));
////}
////
////// Create new subscription entry.
////code notification_worker::create(const incoming& request, send_handler handler)
////{
////    subscription_record record;
////
////    if (!deserialize(record.prefix, record.type, request.data))
////        return error::bad_stream;
////
////    record.expiry_time = now() + settings_.subscription_expiration();
////    record.locator.handler = std::move(handler);
////    record.locator.address1 = request.address1;
////    record.locator.address2 = request.address2;
////    record.locator.delimited = request.delimited;
////
////    ///////////////////////////////////////////////////////////////////////////
////    // Critical Section
////    mutex_.lock_upgrade();
////
////    if (subscriptions_.size() >= settings_.subscription_limit)
////    {
////        mutex_.unlock_upgrade();
////        //---------------------------------------------------------------------
////        return error::pool_filled;
////    }
////
////    mutex_.unlock_upgrade_and_lock();
////    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
////    subscriptions_.emplace_back(record);
////
////    mutex_.unlock();
////    ///////////////////////////////////////////////////////////////////////////
////
////    // This is the end of the subscribe sequence.
////    return error::success;
////}
////
////// Renew sequence.
////// ----------------------------------------------------------------------------
////
////void notification_worker::renew(const incoming& request, send_handler handler)
////{
////    const auto ec = update(request, handler);
////
////    // Send response.
////    handler(outgoing(request, error::success));
////}
////
////// Find subscription record and update expiration.
////code notification_worker::update(const incoming& request, send_handler handler)
////{
////    binary prefix;
////    subscribe_type type;
////
////    if (!deserialize(prefix, type, request.data))
////        return error::bad_stream;
////
////    ///////////////////////////////////////////////////////////////////////////
////    // Critical Section
////    mutex_.lock_upgrade();
////
////    const auto expiry_time = now() + settings_.subscription_expiration();
////
////    for (auto& subscription: subscriptions_)
////    {
////        // TODO: is address1 correct and sufficient?
////        if (subscription.type != type ||
////            subscription.locator.address1 != request.address1 ||
////            !subscription.prefix.is_prefix_of(prefix))
////        {
////            continue;
////        }
////
////        mutex_.unlock_upgrade_and_lock();
////        //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
////        subscription.expiry_time = expiry_time;
////        //---------------------------------------------------------------------
////        mutex_.unlock_and_lock_upgrade();
////    }
////
////    mutex_.unlock_upgrade();
////    ///////////////////////////////////////////////////////////////////////////
////
////    // This is the end of the renew sequence.
////    return error::success;
////}
////
////// Pruning sequence.
////// ----------------------------------------------------------------------------
////
////// Delete entries that have expired.
////size_t notification_worker::prune()
////{
////    ///////////////////////////////////////////////////////////////////////////
////    // Critical Section
////    unique_lock(mutex_);
////
////    const auto current_time = now();
////    const auto start_size = subscriptions_.size();
////
////    for (auto it = subscriptions_.begin(); it != subscriptions_.end();)
////    {
////        if (current_time < it->expiry_time)
////            ++it;
////        else
////            it = subscriptions_.erase(it);
////    }
////
////    // This is the end of the pruning sequence.
////    return start_size - subscriptions_.size();
////    ///////////////////////////////////////////////////////////////////////////
////}
////
////// Scan sequence.
////// ----------------------------------------------------------------------------
////
////void notification_worker::receive_block(uint32_t height, const block::ptr block)
////{
////    const auto hash = block->header.hash();
////
////    for (const auto& transaction: block->transactions)
////        scan(height, hash, transaction);
////
////    prune();
////}
////
////void notification_worker::receive_transaction(const transaction& transaction)
////{
////    scan(0, null_hash, transaction);
////}
////
////void notification_worker::scan(uint32_t height, const hash_digest& block_hash,
////    const transaction& tx)
////{
////    for (const auto& input: tx.inputs)
////    {
////        const auto address = payment_address::extract(input.script);
////
////        if (address)
////            post_updates(address, height, block_hash, tx);
////    }
////
////    uint32_t prefix;
////    for (const auto& output: tx.outputs)
////    {
////        const auto address = payment_address::extract(output.script);
////
////        if (address)
////            post_updates(address, height, block_hash, tx);
////        else if (to_stealth_prefix(prefix, output.script))
////            post_stealth_updates(prefix, height, block_hash, tx);
////    }
////}
////
////void notification_worker::post_updates(const payment_address& address,
////    uint32_t height, const hash_digest& block_hash, const transaction& tx)
////{
////    subscription_locators locators;
////
////    ///////////////////////////////////////////////////////////////////////////
////    // Critical Section
////    mutex_.lock_shared();
////
////    for (const auto& subscription: subscriptions_)
////        if (subscription.type == subscribe_type::address &&
////            subscription.prefix.is_prefix_of(address.hash()))
////                locators.push_back(subscription.locator);
////
////    mutex_.unlock_shared();
////    ///////////////////////////////////////////////////////////////////////////
////
////    if (locators.empty())
////        return;
////
////    // [ address.version:1 ]
////    // [ address.hash:20 ]
////    // [ height:4 ]
////    // [ block_hash:32 ]
////    // [ tx ]
////    const auto data = build_chunk(
////    {
////        to_array(address.version()),
////        address.hash(),
////        to_little_endian(height),
////        block_hash,
////        tx.to_data()
////    });
////
////    // Send the result to everyone interested.
////    for (const auto& locator: locators)
////        locator.handler(outgoing("address.update", data, locator.address1,
////            locator.address2, locator.delimited));
////
////    // This is the end of the scan address sequence.
////}
////
////void notification_worker::post_stealth_updates(uint32_t prefix, uint32_t height,
////    const hash_digest& block_hash, const transaction& tx)
////{
////    subscription_locators locators;
////
////    ///////////////////////////////////////////////////////////////////////////
////    // Critical Section
////    mutex_.lock_shared();
////
////    for (const auto& subscription: subscriptions_)
////        if (subscription.type == subscribe_type::stealth &&
////            subscription.prefix.is_prefix_of(prefix))
////                locators.push_back(subscription.locator);
////
////    mutex_.unlock_shared();
////    ///////////////////////////////////////////////////////////////////////////
////
////    if (locators.empty())
////        return;
////
////    // [ prefix:4 ]
////    // [ height:4 ] 
////    // [ block_hash:32 ]
////    // [ tx ]
////    const auto data = build_chunk(
////    {
////        to_little_endian(prefix),
////        to_little_endian(height),
////        block_hash,
////        tx.to_data()
////    });
////
////    // Send the result to everyone interested.
////    for (const auto& locator: locators)
////        locator.handler(outgoing("address.stealth_update", data,
////            locator.address1, locator.address2, locator.delimited));
////
////    // This is the end of the scan stealth sequence.
////}
////
////// Utilities
////// ----------------------------------------------------------------------------
////
////ptime notification_worker::now()
////{
////    return second_clock::universal_time();
////};

} // namespace server
} // namespace libbitcoin
