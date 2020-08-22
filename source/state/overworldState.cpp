#include "state_impl.hpp"


void OverworldState::exit(Platform& pfrm, Game&, State&)
{
    notification_text.reset();
    fps_text_.reset();
    network_tx_msg_text_.reset();
    network_rx_msg_text_.reset();
    network_tx_loss_text_.reset();
    network_rx_loss_text_.reset();
    link_saturation_text_.reset();
    scratch_buf_avail_text_.reset();

    // In case we're in the middle of an entry/exit animation for the
    // notification bar.
    for (int i = 0; i < 32; ++i) {
        pfrm.set_tile(Layer::overlay, i, 0, 0);
    }

    notification_status = NotificationStatus::hidden;
}


void OverworldState::receive(const net_event::NewLevelIdle& n,
                             Platform& pfrm,
                             Game& game)
{
    if (++idle_rx_count_ == 120) {
        idle_rx_count_ = 0;

        push_notification(pfrm,
                          game.state(),
                          locale_string(LocaleString::peer_transport_waiting));
    }
}


void OverworldState::receive(const net_event::ItemChestShared& s,
                             Platform& pfrm,
                             Game& game)
{
    bool id_collision = false;
    game.details().transform([&](auto& buf) {
        for (auto& e : buf) {
            if (e->id() == s.id_.get()) {
                id_collision = true;
            }
        }
    });
    if (id_collision) {
        error(pfrm, "failed to receive shared item chest, ID collision!");
        return;
    }

    if (game.peer() and
        create_item_chest(game, game.peer()->get_position(), s.item_, false)) {
        pfrm.speaker().play_sound("dropitem", 3);
        (*game.details().get<ItemChest>().begin())->override_id(s.id_.get());
    } else {
        error(pfrm, "failed to allocate shared item chest");
    }
}


void OverworldState::receive(const net_event::PlayerSpawnLaser& p,
                             Platform&,
                             Game& game)
{
    Vec2<Float> position;
    position.x = p.x_.get();
    position.y = p.y_.get();

    game.effects().spawn<PeerLaser>(position, p.dir_, Laser::Mode::normal);
}


void OverworldState::receive(const net_event::SyncSeed& s, Platform&, Game&)
{
    rng::critical_state = s.random_state_.get();
}


void OverworldState::receive(const net_event::EnemyStateSync& s,
                             Platform&,
                             Game& game)
{
    for (auto& e : game.enemies().get<Turret>()) {
        if (e->id() == s.id_.get()) {
            e->sync(s, game);
            return;
        }
    }
    for (auto& e : game.enemies().get<Dasher>()) {
        if (e->id() == s.id_.get()) {
            e->sync(s);
            return;
        }
    }
    for (auto& e : game.enemies().get<Scarecrow>()) {
        if (e->id() == s.id_.get()) {
            e->sync(s);
        }
    }
    for (auto& e : game.enemies().get<Drone>()) {
        if (e->id() == s.id_.get()) {
            e->sync(s);
            return;
        }
    }
}


void OverworldState::receive(const net_event::PlayerInfo& p,
                             Platform&,
                             Game& game)
{
    if (game.peer()) {
        game.peer()->sync(game, p);
    }
}


void OverworldState::receive(const net_event::EnemyHealthChanged& hc,
                             Platform& pfrm,
                             Game& game)
{
    game.enemies().transform([&](auto& buf) {
        for (auto& e : buf) {
            if (e->id() == hc.id_.get()) {
                e->health_changed(hc, pfrm, game);
            }
        }
    });
}


static void transmit_player_info(Platform& pfrm, Game& game)
{
    net_event::PlayerInfo info;
    info.header_.message_type_ = net_event::Header::player_info;
    info.x_.set(game.player().get_position().cast<s16>().x);
    info.y_.set(game.player().get_position().cast<s16>().y);
    info.set_texture_index(game.player().get_sprite().get_texture_index());
    info.set_sprite_size(game.player().get_sprite().get_size());
    info.x_speed_ = game.player().get_speed().x * 10;
    info.y_speed_ = game.player().get_speed().y * 10;
    info.set_visible(game.player().get_sprite().get_alpha() not_eq
                     Sprite::Alpha::transparent);
    info.set_weapon_drawn(game.player().weapon().get_sprite().get_alpha() not_eq
                          Sprite::Alpha::transparent);

    auto mix = game.player().get_sprite().get_mix();
    if (mix.amount_) {
        auto& zone_info = current_zone(game);
        if (mix.color_ == zone_info.injury_glow_color_) {
            info.set_display_color(
                net_event::PlayerInfo::DisplayColor::injured);
        } else if (mix.color_ == zone_info.energy_glow_color_) {
            info.set_display_color(
                net_event::PlayerInfo::DisplayColor::got_coin);
        } else if (mix.color_ == ColorConstant::spanish_crimson) {
            info.set_display_color(
                net_event::PlayerInfo::DisplayColor::got_heart);
        }
        info.set_color_amount(mix.amount_);
    } else {
        info.set_display_color(net_event::PlayerInfo::DisplayColor::none);
        info.set_color_amount(0);
    }

    pfrm.network_peer().send_message({(byte*)&info, sizeof info});
}


void OverworldState::multiplayer_sync(Platform& pfrm,
                                      Game& game,
                                      Microseconds delta)
{
    // On the gameboy advance, we're dealing with a slow connection and
    // twenty-year-old technology, so, realistically, we can only transmit
    // player data a few times per second. TODO: add methods for querying the
    // uplink performance limits from the Platform class, rather than having
    // various game-specific hacks here.

    // FIXME: I strongly suspect that this number can be increased without
    // issues, but I'm waiting until I test on an actual device with a link
    // cable.
    static const auto player_refresh_rate = seconds(1) / 20;

    static auto update_counter = player_refresh_rate;

    if (not game.peer()) {
        game.peer().emplace();
    }

    update_counter -= delta;
    if (update_counter <= 0) {
        update_counter = player_refresh_rate;

        transmit_player_info(pfrm, game);

        if (pfrm.network_peer().is_host()) {
            net_event::SyncSeed s;
            s.random_state_.set(rng::critical_state);
            net_event::transmit(pfrm, s);
        }
    }


    net_event::poll_messages(pfrm, game, *this);


    game.peer()->update(pfrm, game, delta);
}


void player_death(Platform& pfrm, Game& game, const Vec2<Float>& position)
{
    pfrm.speaker().play_sound("explosion1", 3, position);
    big_explosion(pfrm, game, position);
}


void OverworldState::show_stats(Platform& pfrm, Game& game, Microseconds delta)
{
    fps_timer_ += delta;
    fps_frame_count_ += 1;

    if (fps_timer_ >= seconds(1)) {
        fps_timer_ -= seconds(1);

        fps_text_.emplace(pfrm, OverlayCoord{1, 2});
        link_saturation_text_.emplace(pfrm, OverlayCoord{1, 3});
        network_tx_msg_text_.emplace(pfrm, OverlayCoord{1, 4});
        network_rx_msg_text_.emplace(pfrm, OverlayCoord{1, 5});
        network_tx_loss_text_.emplace(pfrm, OverlayCoord{1, 6});
        network_rx_loss_text_.emplace(pfrm, OverlayCoord{1, 7});
        scratch_buf_avail_text_.emplace(pfrm, OverlayCoord{1, 8});

        const auto colors =
            fps_frame_count_ < 55
                ? Text::OptColors{{ColorConstant::rich_black,
                                   ColorConstant::aerospace_orange}}
                : Text::OptColors{};

        fps_text_->assign(fps_frame_count_, colors);
        fps_text_->append(" fps", colors);
        fps_frame_count_ = 0;

        const auto net_stats = pfrm.network_peer().stats();

        const auto tx_loss_colors =
            net_stats.transmit_loss_ > 0
                ? Text::OptColors{{ColorConstant::rich_black,
                                   ColorConstant::aerospace_orange}}
                : Text::OptColors{};

        const auto rx_loss_colors =
            net_stats.receive_loss_ > 0
                ? Text::OptColors{{ColorConstant::rich_black,
                                   ColorConstant::aerospace_orange}}
                : Text::OptColors{};

        network_tx_msg_text_->append(net_stats.transmit_count_);
        network_tx_msg_text_->append(
            locale_string(LocaleString::network_tx_stats_suffix));
        network_rx_msg_text_->append(net_stats.receive_count_);
        network_rx_msg_text_->append(
            locale_string(LocaleString::network_rx_stats_suffix));
        network_tx_loss_text_->append(net_stats.transmit_loss_, tx_loss_colors);
        network_tx_loss_text_->append(
            locale_string(LocaleString::network_tx_loss_stats_suffix),
            tx_loss_colors);
        network_rx_loss_text_->append(net_stats.receive_loss_, rx_loss_colors);
        network_rx_loss_text_->append(
            locale_string(LocaleString::network_rx_loss_stats_suffix),
            rx_loss_colors);
        link_saturation_text_->append(net_stats.link_saturation_);
        link_saturation_text_->append(
            locale_string(LocaleString::link_saturation_stats_suffix));
        scratch_buf_avail_text_->append(pfrm.scratch_buffers_remaining());
        scratch_buf_avail_text_->append(
            locale_string(LocaleString::scratch_buf_avail_stats_suffix));
    }
}


StatePtr OverworldState::update(Platform& pfrm, Game& game, Microseconds delta)
{
    animate_starfield(pfrm, delta);

    if (pfrm.network_peer().is_connected()) {
        multiplayer_sync(pfrm, game, delta);
    } else if (game.peer()) {
        player_death(pfrm, game, game.peer()->get_position());
        game.peer().reset();
        push_notification(
            pfrm, game.state(), locale_string(LocaleString::peer_lost));
    } else {
        // In multiplayer mode, we need to synchronize the random number
        // engine. In single-player mode, let's advance the rng for each step,
        // to add unpredictability
        rng::get(rng::critical_state);
    }

    if (game.persistent_data().settings_.show_stats_) {
        show_stats(pfrm, game, delta);
    } else if (fps_text_) {
        fps_text_.reset();
        network_tx_msg_text_.reset();
        network_tx_loss_text_.reset();
        network_rx_loss_text_.reset();

        fps_frame_count_ = 0;
        fps_timer_ = 0;
    }

    Player& player = game.player();

    auto update_policy = [&](auto& entity_buf) {
        for (auto it = entity_buf.begin(); it not_eq entity_buf.end();) {
            if (not(*it)->alive()) {
                (*it)->on_death(pfrm, game);
                it = entity_buf.erase(it);
            } else {
                (*it)->update(pfrm, game, delta);
                ++it;
            }
        }
    };

    game.effects().transform(update_policy);
    game.details().transform(update_policy);

    auto enemy_timestep = delta;
    if (auto lethargy = get_powerup(game, Powerup::Type::lethargy)) {
        if (lethargy->parameter_ > 0) {
            // FIXME: Improve this code! Division! Yikes!
            const auto last = lethargy->parameter_ / 1000000;
            lethargy->parameter_ -= delta;
            const auto current = lethargy->parameter_ / 1000000;
            if (current not_eq last) {
                lethargy->dirty_ = true;
            }
            enemy_timestep /= 2;
        }
    }

    if (pfrm.keyboard().up_transition(game.action1_key())) {
        camera_snap_timer_ = seconds(2) + milliseconds(250);
    }

    if (camera_snap_timer_ > 0) {
        if (pfrm.keyboard().down_transition(game.action2_key())) {
            camera_snap_timer_ = 0;
        }
        camera_snap_timer_ -= delta;
    }

    const bool boss_level = is_boss_level(game.level());

    auto bosses_remaining = [&] {
        return boss_level and (length(game.enemies().get<Wanderer>()) or
                               length(game.enemies().get<Gatekeeper>()));
    };

    const auto [boss_position, boss_defeated_text] =
        [&]() -> std::pair<Vec2<Float>, LocaleString> {
        if (boss_level) {
            Vec2<Float> result;
            LocaleString lstr = LocaleString::empty;
            for (auto& elem : game.enemies().get<Wanderer>()) {
                result = elem->get_position();
                lstr = elem->defeated_text();
            }
            for (auto& elem : game.enemies().get<Gatekeeper>()) {
                result = elem->get_position();
                lstr = elem->defeated_text();
            }
            return {result, lstr};
        } else {
            return {{}, LocaleString::empty};
        }
    }();

    const bool bosses_were_remaining = bosses_remaining();

    bool enemies_remaining = false;
    bool enemies_destroyed = false;
    bool enemies_visible = false;
    game.enemies().transform([&](auto& entity_buf) {
        for (auto it = entity_buf.begin(); it not_eq entity_buf.end();) {
            if (not(*it)->alive()) {
                (*it)->on_death(pfrm, game);
                it = entity_buf.erase(it);
                enemies_destroyed = true;
            } else {
                enemies_remaining = true;

                (*it)->update(pfrm, game, enemy_timestep);

                if (camera_tracking_ and
                    (pfrm.keyboard().pressed(game.action1_key()) or
                     camera_snap_timer_ > 0)) {
                    // NOTE: snake body segments do not make much sense to
                    // center the camera on, so exclude them. Same for various
                    // other enemies...
                    using T = typename std::remove_reference<decltype(
                        entity_buf)>::type;

                    using VT = typename T::ValueType::element_type;

                    if constexpr (not std::is_same<VT, SnakeBody>() and
                                  not std::is_same<VT, SnakeHead>() and
                                  not std::is_same<VT, GatekeeperShield>()) {
                        if ((*it)->visible()) {
                            enemies_visible = true;
                            game.camera().push_ballast((*it)->get_position());
                        }
                    }
                }
                ++it;
            }
        }
    });

    if (not enemies_visible) {
        camera_snap_timer_ = 0;
    }

    if (not enemies_remaining and enemies_destroyed) {

        NotificationStr str;
        str += locale_string(LocaleString::level_clear);

        push_notification(pfrm, game.state(), str);
    }


    switch (notification_status) {
    case NotificationStatus::hidden:
        break;

    case NotificationStatus::flash:
        for (int x = 0; x < 32; ++x) {
            pfrm.set_tile(Layer::overlay, x, 0, 108);
        }
        notification_status = NotificationStatus::flash_animate;
        notification_text_timer = -1 * milliseconds(5);
        break;

    case NotificationStatus::flash_animate:
        notification_text_timer += delta;
        if (notification_text_timer > milliseconds(10)) {
            notification_text_timer = 0;

            const auto current_tile = pfrm.get_tile(Layer::overlay, 0, 0);
            if (current_tile < 110) {
                for (int x = 0; x < 32; ++x) {
                    pfrm.set_tile(Layer::overlay, x, 0, current_tile + 1);
                }
            } else {
                notification_status = NotificationStatus::wait;
                notification_text_timer = milliseconds(80);
            }
        }
        break;

    case NotificationStatus::wait:
        notification_text_timer -= delta;
        if (notification_text_timer <= 0) {
            notification_text_timer = seconds(3);

            notification_text.emplace(pfrm, OverlayCoord{0, 0});

            const auto margin =
                centered_text_margins(pfrm, notification_str.length());

            left_text_margin(*notification_text, margin);
            notification_text->append(notification_str.c_str());
            right_text_margin(*notification_text, margin);

            notification_status = NotificationStatus::display;
        }
        break;

    case NotificationStatus::display:
        if (notification_text_timer > 0) {
            notification_text_timer -= delta;

        } else {
            notification_text_timer = 0;
            notification_status = NotificationStatus::exit;

            for (int x = 0; x < 32; ++x) {
                pfrm.set_tile(Layer::overlay, x, 0, 112);
            }
        }
        break;

    case NotificationStatus::exit: {
        notification_text_timer += delta;
        if (notification_text_timer > milliseconds(34)) {
            notification_text_timer = 0;

            const auto tile = pfrm.get_tile(Layer::overlay, 0, 0);
            if (tile < 120) {
                for (int x = 0; x < 32; ++x) {
                    pfrm.set_tile(Layer::overlay, x, 0, tile + 1);
                }
            } else {
                notification_status = NotificationStatus::hidden;
                notification_text.reset();
            }
        }
        break;
    }
    }

    game.camera().update(pfrm, delta, player.get_position());


    check_collisions(pfrm, game, player, game.details().get<Item>());
    check_collisions(pfrm, game, player, game.effects().get<OrbShot>());

    if (UNLIKELY(boss_level)) {
        check_collisions(
            pfrm, game, player, game.effects().get<WandererBigLaser>());
        check_collisions(
            pfrm, game, player, game.effects().get<WandererSmallLaser>());
    }

    game.enemies().transform([&](auto& buf) {
        using T = typename std::remove_reference<decltype(buf)>::type;
        using VT = typename T::ValueType::element_type;

        if (pfrm.network_peer().is_connected()) {
            check_collisions(pfrm, game, game.effects().get<PeerLaser>(), buf);
        }

        check_collisions(pfrm, game, game.effects().get<AlliedOrbShot>(), buf);

        if constexpr (not std::is_same<Scarecrow, VT>() and
                      not std::is_same<SnakeTail, VT>() and
                      not std::is_same<Sinkhole, VT>()) {
            check_collisions(pfrm, game, player, buf);
        }

        if constexpr (not std::is_same<Sinkhole, VT>()) {
            check_collisions(pfrm, game, game.effects().get<Laser>(), buf);
        }
    });

    if (bosses_were_remaining and not bosses_remaining()) {
        game.effects().transform([](auto& buf) { buf.clear(); });
        pfrm.sleep(10);
        return state_pool().create<BossDeathSequenceState>(
            game, boss_position, boss_defeated_text);
    }

    return null_state();
}