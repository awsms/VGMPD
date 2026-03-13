// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#pragma once

#include "QueueConfig.hxx"
#include "PlayerConfig.hxx"
#include "lib/pcre/UniqueRegex.hxx"

#include <memory>

struct PartitionConfig {
	QueueConfig queue;
	PlayerConfig player;
	std::shared_ptr<UniqueRegex> art_names_regex;

	PartitionConfig() = default;

	explicit PartitionConfig(const ConfigData &config);
};
