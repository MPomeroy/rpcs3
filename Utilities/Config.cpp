#include "stdafx.h"
#include "Config.h"

#include "yaml-cpp/yaml.h"

namespace cfg
{
	logs::channel cfg("CFG");

	_base::_base(type _type)
		: m_type(_type)
	{
		if (_type != type::node)
		{
			fmt::throw_exception<std::logic_error>("Invalid root node" HERE);
		}
	}

	_base::_base(type _type, node* owner, const std::string& name)
		: m_type(_type)
	{
		for (const auto& pair : owner->m_nodes)
		{
			if (pair.first == name)
			{
				fmt::throw_exception<std::logic_error>("Node already exists: %s" HERE, name);
			}
		}

		owner->m_nodes.emplace_back(name, this);
	}

	bool _base::from_string(const std::string&)
	{
		fmt::throw_exception<std::logic_error>("from_string() purecall" HERE);
	}

	bool _base::from_list(std::vector<std::string>&&)
	{
		fmt::throw_exception<std::logic_error>("from_list() purecall" HERE);
	}

	// Emit YAML
	static void encode(YAML::Emitter& out, const class _base& rhs);

	// Incrementally load config entries from YAML::Node.
	// The config value is preserved if the corresponding YAML node doesn't exist.
	static void decode(const YAML::Node& data, class _base& rhs);
}

bool cfg::try_to_int64(s64* out, const std::string& value, s64 min, s64 max)
{
	// TODO: this could be rewritten without exceptions (but it should be as safe as possible and provide logs)
	s64 result;
	std::size_t pos;

	try
	{
		result = std::stoll(value, &pos, 0 /* Auto-detect numeric base */);
	}
	catch (const std::exception& e)
	{
		if (out) cfg.error("cfg::try_to_int('%s'): exception: %s", value, e.what());
		return false;
	}

	if (pos != value.size())
	{
		if (out) cfg.error("cfg::try_to_int('%s'): unexpected characters (pos=%zu)", value, pos);
		return false;
	}

	if (result < min || result > max)
	{
		if (out) cfg.error("cfg::try_to_int('%s'): out of bounds (%lld..%lld)", value, min, max);
		return false;
	}

	if (out) *out = result;
	return true;
}

bool cfg::try_to_enum_value(u64* out, decltype(&fmt_class_string<int>::format) func, const std::string& value)
{
	for (u64 i = 0;; i++)
	{
		std::string var;
		func(var, i);

		if (var == value)
		{
			if (out) *out = i;
			return true;
		}

		std::string hex;
		fmt_class_string<u64>::format(hex, i);
		if (var == hex)
		{
			break;
		}
	}

	try
	{
		std::size_t pos;
		const auto val = std::stoull(value, &pos, 0);

		if (pos != value.size())
		{
			return false;
		}

		if (out) *out = val;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

std::vector<std::string> cfg::try_to_enum_list(decltype(&fmt_class_string<int>::format) func)
{
	std::vector<std::string> result;

	for (u64 i = 0;; i++)
	{
		std::string var;
		func(var, i);

		std::string hex;
		fmt_class_string<u64>::format(hex, i);
		if (var == hex)
		{
			break;
		}

		result.emplace_back(std::move(var));
	}

	return result;
}

void cfg::encode(YAML::Emitter& out, const cfg::_base& rhs)
{
	switch (rhs.get_type())
	{
	case type::node:
	{
		out << YAML::BeginMap;
		for (const auto& np : static_cast<const node&>(rhs).get_nodes())
		{
			out << YAML::Key << np.first;
			out << YAML::Value;
			encode(out, *np.second);
		}

		out << YAML::EndMap;
		return;
	}
	case type::set:
	{
		out << YAML::BeginSeq;
		for (const auto& str : static_cast<const set_entry&>(rhs).get_set())
		{
			out << str;
		}

		out << YAML::EndSeq;
		return;
	}
	case type::log:
	{
		out << YAML::BeginMap;
		for (const auto& np : static_cast<const log_entry&>(rhs).get_map())
		{
			if (np.second == logs::level::notice) continue;
			out << YAML::Key << np.first;
			out << YAML::Value << fmt::format("%s", np.second);
		}

		out << YAML::EndMap;
		return;
	}
	}

	out << rhs.to_string();
}

void cfg::decode(const YAML::Node& data, cfg::_base& rhs)
{
	switch (rhs.get_type())
	{
	case type::node:
	{
		if (data.IsScalar() || data.IsSequence())
		{
			return; // ???
		}

		for (const auto& pair : data)
		{
			if (!pair.first.IsScalar()) continue;

			// Find the key among existing nodes
			for (const auto& _pair : static_cast<node&>(rhs).get_nodes())
			{
				if (_pair.first == pair.first.Scalar())
				{
					decode(pair.second, *_pair.second);
				}
			}
		}

		break;
	}
	case type::set:
	{
		std::vector<std::string> values;

		if (YAML::convert<decltype(values)>::decode(data, values))
		{
			rhs.from_list(std::move(values));
		}

		break;
	}
	case type::log:
	{
		if (data.IsScalar() || data.IsSequence())
		{
			return; // ???
		}

		std::map<std::string, logs::level> values;

		for (const auto& pair : data)
		{
			if (!pair.first.IsScalar() || !pair.second.IsScalar()) continue;

			u64 value;
			if (cfg::try_to_enum_value(&value, &fmt_class_string<logs::level>::format, pair.second.Scalar()))
			{
				values.emplace(pair.first.Scalar(), static_cast<logs::level>(static_cast<int>(value)));
			}
		}

		static_cast<log_entry&>(rhs).set_map(std::move(values));
		break;
	}
	default:
	{
		std::string value;

		if (YAML::convert<std::string>::decode(data, value))
		{
			rhs.from_string(value);
		}

		break; // ???
	}
	}
}

std::string cfg::node::to_string() const
{
	YAML::Emitter out;
	cfg::encode(out, *this);

	return{ out.c_str(), out.size() };
}

bool cfg::node::from_string(const std::string& value)
{
	cfg::decode(YAML::Load(value), *this);
	return true;
}

void cfg::node::from_default()
{
	for (auto& node : m_nodes)
	{
		node.second->from_default();
	}
}

void cfg::_bool::from_default()
{
	m_value = def;
}

void cfg::string::from_default()
{
	m_value = def;
}

void cfg::set_entry::from_default()
{
	m_set = {};
}

void cfg::log_entry::set_map(std::map<std::string, logs::level>&& map)
{
	logs::reset();

	for (auto&& pair : (m_map = std::move(map)))
	{
		logs::set_level(pair.first, pair.second);
	}
}

void cfg::log_entry::from_default()
{
	set_map({});
}
