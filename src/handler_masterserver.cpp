// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "handler_masterserver.hpp"
#include "global.hpp"
#include "data_model.hpp"
#include "handler_gameserver.hpp"
#include "server_manager.hpp"
#include "authentication.hpp"
#include "hookpoints.hpp"
#include "lobby.hpp"

#include <string>
#include <random>
#include <algorithm>
#include <luxon/ser_interface.hpp>
#include <luxon/common_codes.hpp>
#include <tracy/Tracy.hpp>

// This is a very valuable ressource: https://doc.photonengine.com/realtime/current/lobby-and-matchmaking/matchmaking-and-lobby (2026-02-12)
// http://web.archive.org/web/20260212131901/https://doc.photonengine.com/realtime/current/lobby-and-matchmaking/matchmaking-and-lobby

namespace server {
namespace {
std::string generate_game_id(std::string prefix) {
    static std::mt19937 gen{std::random_device{}()};
    const std::string_view charset = "0123456789";
    std::uniform_int_distribution<size_t> dist(0, charset.size() - 1);

    std::string suffix(4, '\0');
    std::ranges::generate(suffix, [&] { return charset[dist(gen)]; });
    return prefix + '#' + suffix;
}
} // namespace

namespace models {
using namespace DictKeyCodes;

using ClientSettings = Model<Parameter<bool, AuthAndLobby::LobbyStats, true>>;

using LobbyId = Model<Parameter<std::string, AuthAndLobby::LobbyName, false, DefaultString<"">>,
                      Parameter<LobbyType::Enum, AuthAndLobby::LobbyType, false, DefaultConst<LobbyType::Default>>>;

using CreateGame = Model<Parameter<std::string, GameAndActor::GameId, false, DefaultString<"">>>;
using JoinGame = ExtendedModel<CreateGame, Parameter<uint8_t, AuthAndLobby::CreateIfNotExists, false, DefaultConst<false>>>;

using SqlQuery = Model<Parameter<std::string, RoutingAndEvents::Data, false, DefaultString<"">>>;

using JoinRandomGame = ExtendedModel<SqlQuery, Parameter<MatchmakingType::Enum, LoadBalancing::MatchmakingType, false, DefaultInit>,
                                     Parameter<ser::HashtablePtr, Properties::GameProperties, false, DefaultInit>,
                                     Parameter<uint8_t, AuthAndLobby::CreateIfNotExists, false, DefaultConst<false>>,
                                     Parameter<std::string, GameAndActor::GameId, false, DefaultString<"">>>;

using FindFriends = Model<Parameter<std::vector<std::string>, AuthAndLobby::FindFriendsRequestList, false>,
                          Parameter<int32_t, AuthAndLobby::FindFriendsOptions, false, DefaultConst<FindFriendsOptions::Default>>>;

using LobbyStats = Model<Parameter<std::string, DictKeyCodes::AuthAndLobby::LobbyName, true>, Parameter<uint8_t, DictKeyCodes::AuthAndLobby::LobbyType, true>>;
} // namespace models

void MasterServerHandler::HandleSlowUpdate() {
    ZoneScoped;

    if (last_batched_update_.get() > 5000) {
        // Send app stats
        send_app_stats();

        // Batch game list updates
        if (!pending_game_list_updates_.empty()) {
            constexpr size_t max_entries = 500;

            auto game_list = std::make_shared<ser::Hashtable>();
            for (const auto& [game_variant] : pending_game_list_updates_) {
                switch (static_cast<GameListUpdate::Type>(game_variant.index())) {
                case GameListUpdate::Update: {
                    const auto game = std::get<std::weak_ptr<Game>>(game_variant).lock();
                    if (game && game->is_visible)
                        if (auto [element, inserted] = game_list->emplace(game->id, ser::null); inserted)
                            element->second = std::make_shared<ser::Hashtable>(game->get_lobby_game_props());
                } break;
                case GameListUpdate::Delete: {
                    const auto& game_id = std::get<std::string>(game_variant);
                    auto& game_props = *((*game_list)[game_id] = std::make_shared<ser::Hashtable>()).get<ser::HashtablePtr>();
                    game_props[GameProps::Removed] = true;
                }
                }
            }
            pending_game_list_updates_.clear();

            // Send game list updates
            ser::EventMessage event;
            event.event_code = EventCodes::GameList;
            event.parameters[DictKeyCodes::LoadBalancing::GameList] = std::move(game_list);
            send(proto_->Serialize(event));
        }

        last_batched_update_.reset();
    }

    return HandlerBase::HandleSlowUpdate();
}

Awaitable<> MasterServerHandler::HandleOperationRequest(ser::OperationRequestMessage&& req, bool is_encrypted, const enet::EnetCommandHeader& cmd_header) {
    ZoneScoped;

    if (cmd_header.channel_id != 0)
        lco_return lco_await HandlerBase::HandleOperationRequest(std::move(req), is_encrypted, cmd_header);

    if (!peer_->is_authenticated()) {
        switch (req.operation_code) {

        case OpCodes::Auth::Authenticate:
        case OpCodes::Auth::AuthenticateOnce: {
            ZoneScopedN("HandleOperationRequest_Authenticate");

            const auto params = models::ClientSettings::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            // Does the client want lobby stats?
            const bool wants_lobby_stats = params->get<DictKeyCodes::AuthAndLobby::LobbyStats>().value_or(true);

            // Try to authenticate
            auto resp = lco_await authenticate(server_manager_, *peer_, req, cmd_header);

            // Add details if authentication was successful
            if (resp.return_code == ErrorCodes::Core::Ok)
                resp.parameters[DictKeyCodes::LoadBalancing::Position] = static_cast<int32_t>(0);

            // Send response
            send(proto_->Serialize(resp, is_encrypted));

            // Disconnect on error
            if (!peer_->is_authenticated()) {
                peer_->disconnect();
                lco_return;
            }

            // Handle successful authentication
            auto& app = peer_->persistent->app;

            // Fully remove player's reference to current game
            peer_->persistent->reset_game();

            // Send lobby stats if requested
            if (wants_lobby_stats)
                send_lobby_stats();

            lco_return;
        }
        }
    } else {
        const auto get_random_gameserver_base_addr = [this] -> std::string_view {
            return server_manager_.get_random_server_base_address(ServerType::GameServer);
        };

        auto& app = *peer_->persistent->app;

        switch (req.operation_code) {

        case OpCodes::Lobby::JoinLobby: {
            ZoneScopedN("HandleOperationRequest_JoinLobby");

            // Get lobby
            auto joined_lobby = get_requested_lobby(req);
            if (!joined_lobby) {
                send(proto_->Serialize(joined_lobby.error()));
                lco_return;
            }

            // Join the lobby
            join_lobby(std::move(*joined_lobby));
            peer_->log->info("Joined lobby: {}", joined_lobby_->lobby->name.empty() ? "(unnamed)" : joined_lobby_->lobby->name);
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::JoinLobby, .return_code = ErrorCodes::Core::Ok};
            send(proto_->Serialize(resp));

            // Send game list
            send_game_list();
            lco_return;
        }

        case OpCodes::Lobby::LeaveLobby: {
            ZoneScopedN("HandleOperationRequest_LeaveLobby");

            // Try to leave lobby
            std::shared_ptr<Lobby> lobby;
            if (joined_lobby_.has_value()) {
                lobby = joined_lobby_->lobby;
                leave_lobby();
            }

            // Send response (code is always "Ok")
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::LeaveLobby, .return_code = ErrorCodes::Core::Ok};
            if (lobby)
                peer_->log->info("Left lobby: {}", lobby->name.empty() ? "(unnamed)" : lobby->name);
            else
                resp.debug_message = "Lobby not joined";
            send(proto_->Serialize(resp));
            lco_return;
        }

        case OpCodes::Lobby::LobbyStats: {
            ZoneScopedN("HandleOperationRequest_LobbyStats");

            const auto params = models::LobbyStats::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            // Get filters
            const auto *filter_name = params->get<DictKeyCodes::AuthAndLobby::LobbyName>();
            const auto& filter_type = params->get<DictKeyCodes::AuthAndLobby::LobbyType>();

            // Build response
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::LobbyStats};
            resp.parameters = get_lobby_stats([&](const Lobby& lobby) {
                if (filter_name && lobby.name != *filter_name)
                    return false;
                if (filter_type.has_value() && lobby.type != filter_type.value())
                    return false;
                return true;
            });

            // Send response
            send(proto_->Serialize(resp));
            lco_return;
        }

        case OpCodes::Lobby::GetGameList: {
            ZoneScopedN("HandleOperationRequest_GetGameList");

            const auto params = models::SqlQuery::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            // Get lobby
            auto lobby = get_requested_lobby(req);
            if (!lobby) {
                send(proto_->Serialize(lobby.error()));
                lco_return;
            }

            // Error out for non-sql lobbies
            if (lobby.value()->type != LobbyType::SqlLobby) {
                ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::GetGameList,
                                                   .return_code = ErrorCodes::Core::OperationInvalid,
                                                   .debug_message = "Lobby must be SQL lobby type"};
                send(proto_->Serialize(resp));
                lco_return;
            }

            // Build response
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::GetGameList};

            try {
                auto game_ids = lobby.value()->query_lobbies(params->get<DictKeyCodes::RoutingAndEvents::Data>());
                auto game_list = std::make_shared<ser::Hashtable>();

                const auto& games = app.get_games();
                for (const auto& id : game_ids)
                    if (auto res = games.find(id); res != games.end())
                        if (auto game = res->second.lock())
                            game_list->emplace(id, std::make_shared<ser::Hashtable>(game->get_lobby_game_props()));

                resp.parameters[DictKeyCodes::LoadBalancing::GameList] = game_list;
                resp.return_code = ErrorCodes::Core::Ok;
            } catch (const std::exception& e) {
                resp.return_code = ErrorCodes::Core::OperationInvalid;
                resp.debug_message = e.what();
            }

            // Send response
            send(proto_->Serialize(resp));
            lco_return;
        }

        case OpCodes::Matchmaking::CreateGame: {
            ZoneScopedN("HandleOperationRequest_CreateGame");

            const auto params = models::CreateGame::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            std::string game_id = params->get<DictKeyCodes::GameAndActor::GameId>();

            LUXON_SERVER_HOOKPOINT(MasterServer_HandleOperationRequest_CreateGame, game_id);

            // Get lobby
            auto lobby = get_requested_lobby(req);
            if (!lobby) {
                send(proto_->Serialize(lobby.error()));
                lco_return;
            }

            // Generate game ID if empty
            if (game_id.empty())
                game_id = generate_game_id(peer_->persistent->user_id);

            // Make sure no game with given ID already exists
            if (app.get_games().contains(game_id)) {
                const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::CreateGame,
                                                         .return_code = ErrorCodes::Matchmaking::GameIdAlreadyExists,
                                                         .debug_message = "Game ID already exists"};
                send(proto_->Serialize(resp));
                lco_return;
            }

            // Create new game with given ID
            peer_->log->info("Creating game: {}", game_id);
            auto game_expected = lobby.value()->create_game(std::move(game_id), get_random_gameserver_base_addr());
            if (!game_expected) {
                send(proto_->Serialize(game_expected.error()));
                lco_return;
            }
            auto& game = *game_expected;

            // Build response
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::CreateGame, .return_code = ErrorCodes::Core::Ok};
            resp.parameters[DictKeyCodes::LoadBalancing::Address] =
                std::string(server_manager_.resolve_server_address(ServerType::GameServer, peer_->transport_protocol, game->server_address));
            resp.parameters[DictKeyCodes::LoadBalancing::Token] = peer_->persistent->token;
            resp.parameters[DictKeyCodes::GameAndActor::GameId] = game->id;

            // Synchronize game, peer and token
            peer_->log->info("Joining newly created game: {}", game->id);
            peer_->persistent->invite(std::move(game), true);
            sync_persistent_peer(server_manager_, *peer_->persistent);

            // Send response
            send(proto_->Serialize(resp));

            lco_return;
        }

        case OpCodes::Matchmaking::JoinGame: {
            ZoneScopedN("HandleOperationRequest_JoinGame");

            const auto params = models::JoinGame::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            const std::string& game_id = params->get<DictKeyCodes::GameAndActor::GameId>();

            LUXON_SERVER_HOOKPOINT(MasterServer_HandleOperationRequest_JoinGame, game_id, params->get<DictKeyCodes::AuthAndLobby::CreateIfNotExists>());

            // Get lobby
            auto lobby = get_requested_lobby(req);
            if (!lobby) {
                send(proto_->Serialize(lobby.error()));
                lco_return;
            }

            // Find game with given ID
            peer_->log->info("Finding game: {}", game_id);
            const auto& app_games = app.get_games();
            peer_->log->info("Game lookup: id={} present={} entries={}", game_id,
                             app_games.find(game_id) != app_games.end(), app_games.size());

            std::shared_ptr<Game> game;
            bool is_new = false;
            if (auto res = app_games.find(game_id); res == app_games.end()) {
                if (!params->get<DictKeyCodes::AuthAndLobby::CreateIfNotExists>() && !game_id.starts_with("battle-")) {
                    const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinGame,
                                                             .return_code = ErrorCodes::Matchmaking::GameIdNotExists,
                                                             .debug_message = "Game does not exist"};
                    send(proto_->Serialize(resp));
                    lco_return;
                }

                // Generate game ID if empty
                std::string new_game_id;
                if (game_id.empty())
                    new_game_id = generate_game_id(peer_->persistent->user_id);
                else
                    new_game_id = game_id;

                auto game_expected = lobby.value()->create_game(std::move(new_game_id), get_random_gameserver_base_addr());
                if (!game_expected) {
                    send(proto_->Serialize(game_expected.error()));
                    lco_return;
                }
                game = *game_expected;

                is_new = true;
            } else {
                game = res->second.lock();
                peer_->log->info("Existing game lock: id={} alive={}", game_id, static_cast<bool>(game));
            }

            // Make sure game hasn't expired
            if (!game) {
                peer_->log->warn("Game {} expired before JoinGame response", game_id);
                const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinGame,
                                                         .return_code = ErrorCodes::Core::InternalServerError,
                                                         .debug_message = "Game has expired"};
                send(proto_->Serialize(resp));
                lco_return;
            }

            // Validate join
            if (!is_new) {
                peer_->log->info("Validating battle join: id={} created={} open={} peers={} expected={}",
                                 game_id, game->is_created, game->is_open, game->peers.size(), game->expected_users.size());
                const auto [join_validation_code, join_validation_message] = game->validate_join(peer_->persistent->user_id);
                peer_->log->info("Battle join validation: id={} code={}", game_id, join_validation_code);
                // Kick-Flight: both humans of a battle are told the room id at the same time (Stage 3), so the second
                // one usually asks for it before the first has landed on the GameServer and `is_created` is still
                // false. That is not a missing room, only a pending one: let the peer through, the GameServer
                // handler creates/joins on arrival either way (2026-09-21, "Title Disconnect Error" on player 2).
                const bool pending_battle = join_validation_code == ErrorCodes::Matchmaking::GameIdNotExists && !game->is_created && game_id.starts_with("battle-");
                if (pending_battle)
                    peer_->log->info("Game {} is pending creation, allowing join", game_id);
                if (join_validation_code != ErrorCodes::Core::Ok && !pending_battle)
                    peer_->log->warn("Join of game {} rejected: {} ({}); created={} open={} peers={} expected={} max_peers={}",
                                     game_id, join_validation_message, join_validation_code, game->is_created, game->is_open,
                                     game->peers.size(), game->expected_users.size(), game->max_peers);
                if (join_validation_code != ErrorCodes::Core::Ok && !pending_battle) {
                    const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinGame,
                                                             .return_code = join_validation_code,
                                                             .debug_message = std::string(join_validation_message)};
                    send(proto_->Serialize(resp));
                    lco_return;
                }
            }

            // Expect user
            game->expected_users.emplace(peer_->persistent->user_id);

            // Build and send response
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinGame, .return_code = ErrorCodes::Core::Ok};
            resp.parameters[DictKeyCodes::LoadBalancing::Address] =
                std::string(server_manager_.resolve_server_address(ServerType::GameServer, peer_->transport_protocol, game->server_address));
            resp.parameters[DictKeyCodes::LoadBalancing::Token] = peer_->persistent->token;
            if (game->id != game_id)
                resp.parameters[DictKeyCodes::GameAndActor::GameId] = game->id;

            // Synchronize game, peer and token
            peer_->log->info("Joining {} game: {}", is_new ? "newly created" : "existing", game->id);
            peer_->persistent->invite(std::move(game), is_new);
            sync_persistent_peer(server_manager_, *peer_->persistent);

            send(proto_->Serialize(resp));

            lco_return;
        }

        case OpCodes::Matchmaking::JoinRandomGame: {
            ZoneScopedN("HandleOperationRequest_JoinRandomGame");

            const auto params = models::JoinRandomGame::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            // Get lobby
            auto lobby = get_requested_lobby(req);
            if (!lobby) {
                send(proto_->Serialize(lobby.error()));
                lco_return;
            }

            std::shared_ptr<Game> selected_game;

            // Select a matching game
            if (lobby.value()->type == LobbyType::SqlLobby) {
                std::string sql_filter = params->get<DictKeyCodes::RoutingAndEvents::Data>();
                try {
                    auto game_ids = lobby.value()->query_lobbies(sql_filter);
                    for (const auto& id : game_ids) {
                        const auto& app_games = app.get_games();
                        if (auto res = app_games.find(id); res != app_games.end()) {
                            if (auto game = res->second.lock()) {
                                // Make sure game is joinable. validate_join lets closed battle- rooms through for
                                // explicit joins (late joiners); a random join must still never land in a closed room.
                                if (game->is_open && game->validate_join(peer_->persistent->user_id).first == ErrorCodes::Core::Ok) {
                                    selected_game = game;
                                    break;
                                }
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    ser::OperationResponseMessage resp{
                        .operation_code = OpCodes::Matchmaking::JoinRandomGame, .return_code = ErrorCodes::Core::OperationInvalid, .debug_message = e.what()};
                    send(proto_->Serialize(resp));
                    lco_return;
                }
            } else {
                ser::Hashtable expected_props;
                if (auto p = params->get<DictKeyCodes::Properties::GameProperties>())
                    expected_props = *p;

                // Collect candidates
                std::vector<std::shared_ptr<Game>> candidates;
                candidates.reserve(std::min<size_t>(lobby.value()->games.size(), 500)); // Better to allocate more than less?

                for (auto& weak_game : lobby.value()->games) {
                    auto game = weak_game.lock();
                    if (!game)
                        continue;

                    // Make sure game is joinable  TODO: Pass expected user count too
                    // Closed rooms are skipped explicitly: validate_join lets closed battle- rooms through for late joiners.
                    if (!game->is_open || game->validate_join(peer_->persistent->user_id).first != ErrorCodes::Core::Ok)
                        continue;

                    // Property filter
                    if (!game->expect_game_props(expected_props))
                        continue;

                    candidates.push_back(std::move(game));
                }

                // The previous allocation might've been quite a bit overzealous, fix that
                candidates.shrink_to_fit();

                if (!candidates.empty()) {
                    switch (params->get<DictKeyCodes::LoadBalancing::MatchmakingType>()) {
                    case MatchmakingType::SerialMatching: {
                        // Prioritize games with fewer players
                        std::ranges::sort(candidates,
                                          [](const std::shared_ptr<Game>& a, const std::shared_ptr<Game>& b) { return a->peers.size() < b->peers.size(); });
                        selected_game = candidates.front();
                    } break;
                    case MatchmakingType::FillRoom: {
                        // Prioritize games with more players
                        std::ranges::sort(candidates,
                                          [](const std::shared_ptr<Game>& a, const std::shared_ptr<Game>& b) { return a->peers.size() > b->peers.size(); });
                        selected_game = candidates.front();
                    } break;
                    case MatchmakingType::RandomMatching: {
                        // Uniform distribution
                        static std::mt19937 rng(peer_->enet_peer->bytes_out());
                        std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
                        selected_game = candidates[dist(rng)];
                    } break;
                    }
                }
            }

            // Handle no-match condition
            bool is_new = false;
            if (!selected_game) {
                if (!params->get<DictKeyCodes::AuthAndLobby::CreateIfNotExists>()) {
                    const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinRandomGame,
                                                             .return_code = ErrorCodes::Matchmaking::NoRandomMatchFound,
                                                             .debug_message = "No matching game found"};
                    send(proto_->Serialize(resp));
                    lco_return;
                }

                // Generate game ID if empty
                std::string game_id = params->get<DictKeyCodes::GameAndActor::GameId>();
                if (game_id.empty())
                    game_id = generate_game_id(peer_->persistent->user_id);

                // Create new game
                auto game_expected = lobby.value()->create_game(std::move(game_id), get_random_gameserver_base_addr());
                if (!game_expected) {
                    send(proto_->Serialize(game_expected.error()));
                    lco_return;
                }
                selected_game = *game_expected;
                is_new = true;
            }

            // Expect users  TODO: expect all given users
            selected_game->expected_users.emplace(peer_->persistent->user_id);

            // Send Response
            ser::OperationResponseMessage resp;
            resp.operation_code = OpCodes::Matchmaking::JoinRandomGame;
            resp.return_code = ErrorCodes::Core::Ok;

            // Payload similar to Create/Join Game
            resp.parameters[DictKeyCodes::LoadBalancing::Address] =
                std::string(server_manager_.resolve_server_address(ServerType::GameServer, peer_->transport_protocol, selected_game->server_address));
            resp.parameters[DictKeyCodes::LoadBalancing::Token] = peer_->persistent->token;
            resp.parameters[DictKeyCodes::GameAndActor::GameId] = selected_game->id;

            // Synchronize game, peer and token
            peer_->log->info("Matchmaking success. Joining game: {}", selected_game->id);
            peer_->persistent->invite(std::move(selected_game), is_new);
            sync_persistent_peer(server_manager_, *peer_->persistent);

            send(proto_->Serialize(resp));
            lco_return;
        }

        case OpCodes::Social::FindFriends: {
            ZoneScopedN("HandleOperationRequest_FindFriends");

            if (!app.get_settings().allow_find_friends) {
                ser::OperationResponseMessage resp{.operation_code = OpCodes::Social::FindFriends,
                                                   .return_code = ErrorCodes::Core::OperationNotAllowedInCurrentState,
                                                   .debug_message = "FindFriends operation is not allowed."};
                send(proto_->Serialize(resp));
                lco_return;
            }

            const auto params = models::FindFriends::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            const auto& friend_list = params->get<DictKeyCodes::AuthAndLobby::FindFriendsRequestList>();
            const int flags = params->get<DictKeyCodes::AuthAndLobby::FindFriendsOptions>();
            std::vector<bool> online_list;
            std::vector<std::string> room_list;

            online_list.reserve(friend_list.size());
            room_list.reserve(friend_list.size());

            const auto& connections = server_manager_.get_connections();

            for (const auto& friend_id : friend_list) {
                bool is_online = false;
                std::string room_id = "";

                for (const auto& conn : connections) {
                    auto peer_conn = conn->get_peer();
                    if (peer_conn && peer_conn->persistent && peer_conn->persistent->user_id == friend_id) {
                        is_online = true;
                        if (auto expected_game = server_manager_.get_game(*peer_conn->persistent->app, peer_conn->persistent->get_invitation())) {
                            if (auto game = *expected_game) {
                                if ((flags & FindFriendsOptions::CreatedOnGS) && !game->find_peer(peer_conn))
                                    break;
                                if ((flags & FindFriendsOptions::Visible) && !game->is_visible)
                                    break;
                                if ((flags & FindFriendsOptions::Open) && !game->is_open)
                                    break;
                                room_id = game->id;
                            }
                        }
                        break;
                    }
                }
                online_list.push_back(is_online);
                room_list.push_back(room_id);
            }

            ser::OperationResponseMessage resp{.operation_code = OpCodes::Social::FindFriends};

            resp.parameters[DictKeyCodes::AuthAndLobby::FindFriendsResponseOnlineList] = std::move(online_list);
            resp.parameters[DictKeyCodes::AuthAndLobby::FindFriendsResponseRoomIdList] = std::move(room_list);

            send(proto_->Serialize(resp));
            lco_return;
        }
        }
    }

    lco_return lco_await HandlerBase::HandleOperationRequest(std::move(req), is_encrypted, cmd_header);
}

std::expected<std::shared_ptr<Lobby>, ser::OperationResponseMessage> MasterServerHandler::get_requested_lobby(const ser::OperationRequestMessage& req) {
    ZoneScoped;

    const auto lobby_id = models::LobbyId::decode(req);
    if (!lobby_id)
        return std::unexpected(lobby_id.error());
    const std::string& lobby_name = lobby_id->get<DictKeyCodes::AuthAndLobby::LobbyName>();

    if (lobby_name.empty() && joined_lobby_)
        return joined_lobby_->lobby;

    return peer_->persistent->app->get_lobby({lobby_id->get<DictKeyCodes::AuthAndLobby::LobbyName>(), lobby_id->get<DictKeyCodes::AuthAndLobby::LobbyType>()});
}

void MasterServerHandler::join_lobby(std::shared_ptr<Lobby> lobby) {
    ZoneScoped;

    if (lobby->type == LobbyType::Default) {
        joined_lobby_.emplace(std::move(lobby),
                              GameListUpdateHandler{.game_update =
                                                        [this](const std::shared_ptr<Game>& game) {
                                                            if (pending_game_list_updates_.size() < 500)
                                                                pending_game_list_updates_.emplace_back(game);
                                                        },
                                                    .game_delete = [this](Game *game) { pending_game_list_updates_.emplace_back(game->id); }});
    } else {
        joined_lobby_.emplace(std::move(lobby),
                              GameListUpdateHandler{.game_update = [](const std::shared_ptr<Game>& game) {}, .game_delete = [](Game *game) {}});
    }
}

void MasterServerHandler::send_app_stats() {
    ZoneScoped;

    ser::EventMessage event;

    event.event_code = EventCodes::AppStats;
    event.parameters[DictKeyCodes::LoadBalancing::GameCount] = [this]() {
        int32_t fres = 0;
        for (auto&& app : App::get_all(server_manager_))
            for (const auto& [lobby_name, weak_lobby] : app->get_lobbies())
                if (auto lobby = weak_lobby.lock())
                    fres += lobby->games.size();
        return fres;
    }();
    event.parameters[DictKeyCodes::LoadBalancing::PeerCount] = static_cast<int32_t>(server_manager_.get_connection_count<GameServerHandler>());
    event.parameters[DictKeyCodes::LoadBalancing::MasterPeerCount] = static_cast<int32_t>(server_manager_.get_connection_count<MasterServerHandler>());

    send(proto_->Serialize(event), enet::EnetSendOptions{.mode = luxon::enet::EnetDeliveryMode::Unreliable});
}

ser::Dictionary MasterServerHandler::get_lobby_stats(std::function<bool(const Lobby&)> lobby_filter) {
    ZoneScoped;

    ser::Dictionary fres;

    fres[DictKeyCodes::LoadBalancing::PeerCount] = std::vector<int32_t>();
    fres[DictKeyCodes::LoadBalancing::GameCount] = std::vector<int32_t>();
    fres[DictKeyCodes::AuthAndLobby::LobbyType] = std::vector<uint8_t>();
    fres[DictKeyCodes::AuthAndLobby::LobbyName] = std::vector<std::string>();

    auto& peer_count_arr = fres[DictKeyCodes::LoadBalancing::PeerCount].get<std::vector<int32_t>>();
    auto& game_count_arr = fres[DictKeyCodes::LoadBalancing::GameCount].get<std::vector<int32_t>>();
    auto& lobby_type_arr = fres[DictKeyCodes::AuthAndLobby::LobbyType].get<std::vector<uint8_t>>();
    auto& lobby_name_arr = fres[DictKeyCodes::AuthAndLobby::LobbyName].get<std::vector<std::string>>();

    auto& app = *peer_->persistent->app;
    for (const auto& [lobby_name, weak_lobby] : app.get_lobbies()) {
        if (auto lobby = weak_lobby.lock()) {
            if (lobby_filter && !lobby_filter(*lobby))
                continue;

            lobby_name_arr.push_back(lobby->name);
            lobby_type_arr.push_back(lobby->type);
            game_count_arr.push_back(lobby->games.size());
            peer_count_arr.push_back(lobby->get_peer_count());
        }
    }

    return fres;
}

void MasterServerHandler::send_lobby_stats() {
    ZoneScoped;

    ser::EventMessage event;

    event.event_code = EventCodes::LobbyStats;
    event.parameters = get_lobby_stats();

    send(proto_->Serialize(event), enet::EnetSendOptions{.mode = luxon::enet::EnetDeliveryMode::Unreliable});
}

ser::HashtablePtr MasterServerHandler::get_game_list(Lobby& lobby, const Game& game) {
    ZoneScoped;

    auto fres = std::make_shared<ser::Hashtable>();
    if (game.is_visible)
        fres->emplace(game.id, std::make_shared<ser::Hashtable>(game.get_lobby_game_props()));

    return fres;
}

ser::HashtablePtr MasterServerHandler::get_game_list(Lobby& lobby, std::function<bool(const Game&)> game_filter) {
    ZoneScoped;

    constexpr size_t max_entries = 500;

    auto fres = std::make_shared<ser::Hashtable>();

    if (!joined_lobby_.has_value())
        return fres;

    // Collect valid games into a vector
    std::vector<std::shared_ptr<Game>> sorted_games;
    sorted_games.reserve(lobby.games.size());

    for (auto& weak_game : lobby.games) {
        auto game = weak_game.lock();
        if (!game)
            continue;
        if (!game->is_visible)
            continue;
        if (game_filter && !game_filter(*game))
            continue;

        sorted_games.push_back(std::move(game));
    }

    // Shortcut if no game did match
    if (sorted_games.empty())
        return fres;

    // Shortcut if only one game did match
    if (sorted_games.size() == 1) {
        fres->emplace(sorted_games[0]->id, std::make_shared<ser::Hashtable>(sorted_games[0]->get_lobby_game_props()));
        return fres;
    }

    // The list is sorted using two criteria: open or closed, full or not
    std::ranges::sort(sorted_games, [](const std::shared_ptr<Game>& a, const std::shared_ptr<Game>& b) {
        auto get_group = [](const Game& g) {
            // First group: open and not full (joinable).
            if (g.is_open && g.peers.size() < g.max_peers)
                return 0;
            // Third group: closed (not joinable, could be full or not)
            if (!g.is_open)
                return 2;
            // Second group: full but not closed (not joinable)
            return 1;
        };

        int group_a = get_group(*a);
        int group_b = get_group(*b);

        if (group_a != group_b)
            return group_a < group_b;

        return a->id < b->id;
    });

    // Populate final list
    for (size_t i = 0; i < std::min(sorted_games.size(), max_entries); ++i)
        fres->emplace(sorted_games[i]->id, std::make_shared<ser::Hashtable>(sorted_games[i]->get_lobby_game_props()));

    return fres;
}

void MasterServerHandler::send_game_list() {
    ZoneScoped;

    if (!joined_lobby_)
        return;

    ser::EventMessage event;

    event.event_code = EventCodes::GameList;
    event.parameters[DictKeyCodes::LoadBalancing::GameList] = get_game_list(*joined_lobby_->lobby);

    send(proto_->Serialize(event));
}

MasterServerHandler::JoinedLobby::JoinedLobby(std::shared_ptr<Lobby> lobby_, GameListUpdateHandler&& handler) : lobby(std::move(lobby_)) {
    lobby->game_list_update_handlers.emplace_front(std::move(handler));
    game_list_update_handler = lobby->game_list_update_handlers.begin();
}

MasterServerHandler::JoinedLobby::~JoinedLobby() { lobby->game_list_update_handlers.erase(game_list_update_handler); }
} // namespace server
