#include "collection.h"

#include <numeric>
#include <chrono>
#include <array_utils.h>
#include <match_score.h>
#include <string_utils.h>
#include <art.h>

Collection::Collection(const std::string name, const uint32_t collection_id, const uint32_t next_seq_id, Store *store,
                       const std::vector<field> &fields, const std::string & token_ranking_field):
                       name(name), collection_id(collection_id), next_seq_id(next_seq_id), store(store),
                       token_ranking_field(token_ranking_field) {

    for(const field& field: fields) {
        search_schema.emplace(field.name, field);

        if(field.is_facet()) {
            facet_value fvalue;
            facet_schema.emplace(field.name, field);
        }

        if(field.is_single_integer() || field.is_single_float()) {
            sort_schema.emplace(field.name, field);
        }
    }

    num_indices = 4;
    for(auto i = 0; i < num_indices; i++) {
        indices.push_back(new Index(name, search_schema, facet_schema, sort_schema));
    }

    num_documents = 0;
}

Collection::~Collection() {
    for(auto index: indices) {
        delete index;
    }
}

uint32_t Collection::get_next_seq_id() {
    store->increment(get_next_seq_id_key(name), 1);
    return next_seq_id++;
}

void Collection::set_next_seq_id(uint32_t seq_id) {
    next_seq_id = seq_id;
}

void Collection::increment_next_seq_id_field() {
    next_seq_id++;
}

Option<std::string> Collection::add(const std::string & json_str) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(json_str);
    } catch(...) {
        return Option<std::string>(400, "Bad JSON.");
    }

    uint32_t seq_id = get_next_seq_id();
    std::string seq_id_str = std::to_string(seq_id);

    if(document.count("id") == 0) {
        document["id"] = seq_id_str;
    } else if(!document["id"].is_string()) {
        return Option<std::string>(400, "Document's `id` field should be a string.");
    }

    std::string doc_id = document["id"];

    const Option<uint32_t> & index_memory_op = index_in_memory(document, seq_id);

    if(!index_memory_op.ok()) {
        return Option<std::string>(index_memory_op.code(), index_memory_op.error());
    }

    store->insert(get_doc_id_key(document["id"]), seq_id_str);
    store->insert(get_seq_id_key(seq_id), document.dump());

    return Option<std::string>(doc_id);
}

Option<uint32_t> Collection::validate_index_in_memory(const nlohmann::json &document, uint32_t seq_id) {
    if(!token_ranking_field.empty() && document.count(token_ranking_field) == 0) {
        return Option<>(400, "Field `" + token_ranking_field  + "` has been declared as a token ranking field, "
                "but is not found in the document.");
    }

    if(!token_ranking_field.empty() && !document[token_ranking_field].is_number_integer() &&
       !document[token_ranking_field].is_number_float()) {
        return Option<>(400, "Token ranking field `" + token_ranking_field  + "` must be a number.");
    }

    if(!token_ranking_field.empty() && document[token_ranking_field].is_number_integer() &&
       document[token_ranking_field].get<int64_t>() > std::numeric_limits<int32_t>::max()) {
        return Option<>(400, "Token ranking field `" + token_ranking_field  + "` exceeds maximum value of int32.");
    }

    if(!token_ranking_field.empty() && document[token_ranking_field].is_number_float() &&
       document[token_ranking_field].get<float>() > std::numeric_limits<float>::max()) {
        return Option<>(400, "Token ranking field `" + token_ranking_field  + "` exceeds maximum value of a float.");
    }

    for(const std::pair<std::string, field> & field_pair: search_schema) {
        const std::string & field_name = field_pair.first;

        if(document.count(field_name) == 0) {
            return Option<>(400, "Field `" + field_name  + "` has been declared in the schema, "
                    "but is not found in the document.");
        }

        if(field_pair.second.type == field_types::STRING) {
            if(!document[field_name].is_string()) {
                return Option<>(400, "Field `" + field_name  + "` must be a string.");
            }
        } else if(field_pair.second.type == field_types::INT32) {
            if(!document[field_name].is_number_integer()) {
                return Option<>(400, "Field `" + field_name  + "` must be an int32.");
            }

            if(document[field_name].get<int64_t>() > INT32_MAX) {
                return Option<>(400, "Field `" + field_name  + "` exceeds maximum value of int32.");
            }
        } else if(field_pair.second.type == field_types::INT64) {
            if(!document[field_name].is_number_integer()) {
                return Option<>(400, "Field `" + field_name  + "` must be an int64.");
            }
        } else if(field_pair.second.type == field_types::FLOAT) {
            if(!document[field_name].is_number()) { // allows integer to be passed to a float field
                return Option<>(400, "Field `" + field_name  + "` must be a float.");
            }
        } else if(field_pair.second.type == field_types::STRING_ARRAY) {
            if(!document[field_name].is_array()) {
                return Option<>(400, "Field `" + field_name  + "` must be a string array.");
            }
            if(document[field_name].size() > 0 && !document[field_name][0].is_string()) {
                return Option<>(400, "Field `" + field_name  + "` must be a string array.");
            }
        } else if(field_pair.second.type == field_types::INT32_ARRAY) {
            if(!document[field_name].is_array()) {
                return Option<>(400, "Field `" + field_name  + "` must be an int32 array.");
            }

            if(document[field_name].size() > 0 && !document[field_name][0].is_number_integer()) {
                return Option<>(400, "Field `" + field_name  + "` must be an int32 array.");
            }
        } else if(field_pair.second.type == field_types::INT64_ARRAY) {
            if(!document[field_name].is_array()) {
                return Option<>(400, "Field `" + field_name  + "` must be an int64 array.");
            }

            if(document[field_name].size() > 0 && !document[field_name][0].is_number_integer()) {
                return Option<>(400, "Field `" + field_name  + "` must be an int64 array.");
            }
        } else if(field_pair.second.type == field_types::FLOAT_ARRAY) {
            if(!document[field_name].is_array()) {
                return Option<>(400, "Field `" + field_name  + "` must be a float array.");
            }

            if(document[field_name].size() > 0 && !document[field_name][0].is_number_float()) {
                return Option<>(400, "Field `" + field_name  + "` must be a float array.");
            }
        }
    }

    for(const std::pair<std::string, field> & field_pair: facet_schema) {
        const std::string & field_name = field_pair.first;

        if(document.count(field_name) == 0) {
            return Option<>(400, "Field `" + field_name  + "` has been declared as a facet field in the schema, "
                    "but is not found in the document.");
        }

        if(field_pair.second.type == field_types::STRING) {
            if(!document[field_name].is_string()) {
                return Option<>(400, "Facet field `" + field_name  + "` must be a string.");
            }
        } else if(field_pair.second.type == field_types::STRING_ARRAY) {
            if(!document[field_name].is_array()) {
                return Option<>(400, "Facet field `" + field_name  + "` must be a string array.");
            }

            if(document[field_name].size() > 0 && !document[field_name][0].is_string()) {
                return Option<>(400, "Facet field `" + field_name  + "` must be a string array.");
            }
        } else {
            return Option<>(400, "Facet field `" + field_name  + "` must be a string or a string[].");
        }
    }

    return Option<>(200);
}

Option<uint32_t> Collection::index_in_memory(const nlohmann::json &document, uint32_t seq_id) {
    Option<uint32_t> validation_op = validate_index_in_memory(document, seq_id);

    if(!validation_op.ok()) {
        return validation_op;
    }

    int32_t points = 0;

    if(!token_ranking_field.empty()) {
        if(document[token_ranking_field].is_number_float()) {
            // serialize float to an integer and reverse the inverted range
            float n = document[token_ranking_field];
            memcpy(&points, &n, sizeof(int32_t));
            points ^= ((points >> (std::numeric_limits<int32_t>::digits - 1)) | INT32_MIN);
            points = -1 * (INT32_MAX - points);
        } else {
            points = document[token_ranking_field];
        }
    }

    Index* index = indices[seq_id % num_indices];
    index->index_in_memory(document, seq_id, points);

    num_documents += 1;
    return Option<>(200);
}

Option<nlohmann::json> Collection::search(std::string query, const std::vector<std::string> search_fields,
                                  const std::string & simple_filter_query, const std::vector<std::string> & facet_fields,
                                  const std::vector<sort_by> & sort_fields, const int num_typos,
                                  const size_t per_page, const size_t page,
                                  const token_ordering token_order, const bool prefix) {
    std::vector<facet> facets;

    // validate search fields
    for(const std::string & field_name: search_fields) {
        if(search_schema.count(field_name) == 0) {
            std::string error = "Could not find a field named `" + field_name + "` in the schema.";
            return Option<nlohmann::json>(400, error);
        }

        field search_field = search_schema.at(field_name);
        if(search_field.type != field_types::STRING && search_field.type != field_types::STRING_ARRAY) {
            std::string error = "Field `" + field_name + "` should be a string or a string array.";
            return Option<nlohmann::json>(400, error);
        }

        if(search_field.facet) {
            std::string error = "Field `" + field_name + "` is a faceted field - it cannot be used as a query field.";
            return Option<nlohmann::json>(400, error);
        }
    }

    // validate facet fields
    for(const std::string & field_name: facet_fields) {
        if(facet_schema.count(field_name) == 0) {
            std::string error = "Could not find a facet field named `" + field_name + "` in the schema.";
            return Option<nlohmann::json>(400, error);
        }
        facets.push_back(facet(field_name));
    }

    // validate sort fields and standardize

    std::vector<sort_by> sort_fields_std;

    for(const sort_by & _sort_field: sort_fields) {
        if(sort_schema.count(_sort_field.name) == 0) {
            std::string error = "Could not find a field named `" + _sort_field.name + "` in the schema for sorting.";
            return Option<nlohmann::json>(400, error);
        }

        std::string sort_order = _sort_field.order;
        StringUtils::toupper(sort_order);

        if(sort_order != sort_field_const::asc && sort_order != sort_field_const::desc) {
            std::string error = "Order for field` " + _sort_field.name + "` should be either ASC or DESC.";
            return Option<nlohmann::json>(400, error);
        }

        sort_fields_std.push_back({_sort_field.name, sort_order});
    }

    // check for valid pagination
    if(page < 1) {
        std::string message = "Page must be an integer of value greater than 0.";
        return Option<nlohmann::json>(422, message);
    }

    if((page * per_page) > MAX_RESULTS) {
        std::string message = "Only the first " + std::to_string(MAX_RESULTS) + " results are available.";
        return Option<nlohmann::json>(422, message);
    }

    auto begin = std::chrono::high_resolution_clock::now();

    // all search queries that were used for generating the results
    std::vector<std::vector<art_leaf*>> searched_queries;

    std::vector<std::pair<int, Topster<100>::KV>> field_order_kvs;
    size_t all_result_ids_len = 0;

    for(Index* index: indices) {
        index->search(query, search_fields, simple_filter_query, facets, sort_fields_std, num_typos,
                      per_page, page, token_order, prefix, field_order_kvs, all_result_ids_len, searched_queries);
    }

    // All fields are sorted descending
    std::sort(field_order_kvs.begin(), field_order_kvs.end(),
      [](const std::pair<int, Topster<100>::KV> & a, const std::pair<int, Topster<100>::KV> & b) {
          return std::tie(a.second.match_score, a.second.primary_attr, a.second.secondary_attr, a.first, a.second.key) >
                 std::tie(b.second.match_score, b.second.primary_attr, b.second.secondary_attr, b.first, b.second.key);
    });

    nlohmann::json result = nlohmann::json::object();

    result["hits"] = nlohmann::json::array();
    result["found"] = all_result_ids_len;

    const int start_result_index = (page - 1) * per_page;
    const int kvsize = field_order_kvs.size();

    if(start_result_index > (kvsize - 1)) {
        return Option<nlohmann::json>(result);
    }

    const int end_result_index = std::min(int(page * per_page), kvsize) - 1;

    for(size_t field_order_kv_index = start_result_index; field_order_kv_index <= end_result_index; field_order_kv_index++) {
        const auto & field_order_kv = field_order_kvs[field_order_kv_index];
        const std::string& seq_id_key = get_seq_id_key((uint32_t) field_order_kv.second.key);

        std::string value;
        store->get(seq_id_key, value);

        nlohmann::json document;
        try {
            document = nlohmann::json::parse(value);
        } catch(...) {
            return Option<nlohmann::json>(500, "Error while parsing stored document.");
        }

        // highlight query words in the result
        const std::string & field_name = search_fields[search_fields.size() - field_order_kv.first];
        field search_field = search_schema.at(field_name);

        // only string fields are supported for now
        if(search_field.type == field_types::STRING) {
            std::vector<std::string> tokens;
            StringUtils::split(document[field_name], tokens, " ");

            // positions in the document of each token in the query
            std::vector<std::vector<uint16_t>> token_positions;

            for (const art_leaf *token_leaf : searched_queries[field_order_kv.second.query_index]) {
                std::vector<uint16_t> positions;
                int doc_index = token_leaf->values->ids.indexOf(field_order_kv.second.key);
                if(doc_index == token_leaf->values->ids.getLength()) {
                    continue;
                }

                uint32_t start_offset = token_leaf->values->offset_index.at(doc_index);
                uint32_t end_offset = (doc_index == token_leaf->values->ids.getLength() - 1) ?
                                      token_leaf->values->offsets.getLength() :
                                      token_leaf->values->offset_index.at(doc_index+1);

                while(start_offset < end_offset) {
                    positions.push_back((uint16_t) token_leaf->values->offsets.at(start_offset));
                    start_offset++;
                }

                token_positions.push_back(positions);
            }

            MatchScore mscore = MatchScore::match_score(field_order_kv.second.key, token_positions);

            // unpack `mscore.offset_diffs` into `token_indices`
            std::vector<size_t> token_indices;
            char num_tokens_found = mscore.offset_diffs[0];
            for(size_t i = 1; i <= num_tokens_found; i++) {
                if(mscore.offset_diffs[i] != std::numeric_limits<int8_t>::max()) {
                    size_t token_index = (size_t)(mscore.start_offset + mscore.offset_diffs[i]);
                    token_indices.push_back(token_index);
                }
            }

            auto minmax = std::minmax_element(token_indices.begin(), token_indices.end());

            // For longer strings, pick surrounding tokens within N tokens of min_index and max_index for the snippet
            const size_t start_index = (tokens.size() <= SNIPPET_STR_ABOVE_LEN) ? 0 :
                                       std::max(0, (int)(*(minmax.first)-5));

            const size_t end_index = (tokens.size() <= SNIPPET_STR_ABOVE_LEN) ? tokens.size() :
                                     std::min((int)tokens.size(), (int)(*(minmax.second)+5));

            for(const size_t token_index: token_indices) {
                tokens[token_index] = "<mark>" + tokens[token_index] + "</mark>";
            }

            std::stringstream snippet_stream;
            for(size_t snippet_index = start_index; snippet_index < end_index; snippet_index++) {
                if(snippet_index != start_index) {
                    snippet_stream << " ";
                }

                snippet_stream << tokens[snippet_index];
            }

            document["_highlight"] = nlohmann::json::object();
            document["_highlight"][field_name] = snippet_stream.str();
        }

        result["hits"].push_back(document);
    }

    result["facet_counts"] = nlohmann::json::array();

    // populate facets
    for(const facet & a_facet: facets) {
        nlohmann::json facet_result = nlohmann::json::object();
        facet_result["field_name"] = a_facet.field_name;
        facet_result["counts"] = nlohmann::json::array();

        // keep only top 10 facets
        std::vector<std::pair<std::string, size_t>> value_to_count;
        for (auto itr = a_facet.result_map.begin(); itr != a_facet.result_map.end(); ++itr) {
            value_to_count.push_back(*itr);
        }

        std::sort(value_to_count.begin(), value_to_count.end(),
                  [=](std::pair<std::string, size_t>& a, std::pair<std::string, size_t>& b) {
                      return a.second > b.second;
                  });

        for(auto i = 0; i < std::min((size_t)10, value_to_count.size()); i++) {
            auto & kv = value_to_count[i];
            nlohmann::json facet_value_count = nlohmann::json::object();
            facet_value_count["value"] = kv.first;
            facet_value_count["count"] = kv.second;
            facet_result["counts"].push_back(facet_value_count);
        }

        result["facet_counts"].push_back(facet_result);
    }

    long long int timeMillis = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - begin).count();
    //!std::cout << "Time taken for result calc: " << timeMillis << "us" << std::endl;
    //!store->print_memory_usage();
    return result;
}

Option<nlohmann::json> Collection::get(const std::string & id) {
    std::string seq_id_str;
    StoreStatus status = store->get(get_doc_id_key(id), seq_id_str);

    if(status == StoreStatus::NOT_FOUND) {
        return Option<nlohmann::json>(404, "Could not find a document with id: " + id);
    }

    uint32_t seq_id = (uint32_t) std::stol(seq_id_str);

    std::string parsed_document;
    store->get(get_seq_id_key(seq_id), parsed_document);

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(parsed_document);
    } catch(...) {
        return Option<nlohmann::json>(500, "Error while parsing stored document.");
    }

    return Option<nlohmann::json>(document);
}

Option<std::string> Collection::remove(const std::string & id, const bool remove_from_store) {
    std::string seq_id_str;
    StoreStatus status = store->get(get_doc_id_key(id), seq_id_str);

    if(status == StoreStatus::NOT_FOUND) {
        return Option<std::string>(404, "Could not find a document with id: " + id);
    }

    uint32_t seq_id = (uint32_t) std::stol(seq_id_str);

    std::string parsed_document;
    store->get(get_seq_id_key(seq_id), parsed_document);

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(parsed_document);
    } catch(...) {
        return Option<std::string>(500, "Error while parsing stored document.");
    }

    for(Index* index: indices) {
        index->remove(seq_id, document);
    }

    if(remove_from_store) {
        store->remove(get_doc_id_key(id));
        store->remove(get_seq_id_key(seq_id));
    }

    num_documents -= 1;

    return Option<std::string>(id);
}

std::string Collection::get_next_seq_id_key(const std::string & collection_name) {
    return std::string(COLLECTION_NEXT_SEQ_PREFIX) + "_" + collection_name;
}

std::string Collection::get_seq_id_key(uint32_t seq_id) {
    // We can't simply do std::to_string() because we want to preserve the byte order.
    // & 0xFF masks all but the lowest eight bits.
    unsigned char bytes[4];
    bytes[0] = (unsigned char) ((seq_id >> 24) & 0xFF);
    bytes[1] = (unsigned char) ((seq_id >> 16) & 0xFF);
    bytes[2] = (unsigned char) ((seq_id >> 8) & 0xFF);
    bytes[3] = (unsigned char) ((seq_id & 0xFF));

    return get_seq_id_collection_prefix() + "_" + std::string(bytes, bytes+4);
}

uint32_t Collection::deserialize_seq_id_key(std::string serialized_seq_id) {
    uint32_t seq_id = ((serialized_seq_id[0] & 0xFF) << 24) | ((serialized_seq_id[1] & 0xFF) << 16) |
                      ((serialized_seq_id[2] & 0xFF) << 8)  | (serialized_seq_id[3] & 0xFF);
    return seq_id;
}

std::string Collection::get_doc_id_key(const std::string & doc_id) {
    return std::to_string(collection_id) + "_" + DOC_ID_PREFIX + "_" + doc_id;
}

std::string Collection::get_name() {
    return name;
}

size_t Collection::get_num_documents() {
    return num_documents;
}

uint32_t Collection::get_collection_id() {
    return collection_id;
}

uint32_t Collection::doc_id_to_seq_id(std::string doc_id) {
    std::string seq_id_str;
    store->get(get_doc_id_key(doc_id), seq_id_str);
    uint32_t seq_id = (uint32_t) std::stoi(seq_id_str);
    return seq_id;
}

std::vector<std::string> Collection::get_facet_fields() {
    std::vector<std::string> facet_fields_copy;
    for(auto it = facet_schema.begin(); it != facet_schema.end(); ++it) {
        facet_fields_copy.push_back(it->first);
    }

    return facet_fields_copy;
}

std::vector<field> Collection::get_sort_fields() {
    std::vector<field> sort_fields_copy;
    for(auto it = sort_schema.begin(); it != sort_schema.end(); ++it) {
        sort_fields_copy.push_back(it->second);
    }

    return sort_fields_copy;
}

spp::sparse_hash_map<std::string, field> Collection::get_schema() {
    return search_schema;
};

std::string Collection::get_meta_key(const std::string & collection_name) {
    return std::string(COLLECTION_META_PREFIX) + "_" + collection_name;
}

std::string Collection::get_seq_id_collection_prefix() {
    return std::to_string(collection_id) + "_" + std::string(SEQ_ID_PREFIX);
}

std::string Collection::get_token_ranking_field() {
    return token_ranking_field;
}