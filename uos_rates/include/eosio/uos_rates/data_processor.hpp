#pragma once

#include <eosio/uos_rates/uos_rates.hpp>
//#include <eosio/uos_rates/transaction_queqe.hpp>
#include <eosio/uos_rates/merkle_tree.hpp>
//#include <eosio/chain/asset.hpp>
//#include <eosio/chain/exceptions.hpp>
//#include <eosio/chain_api_plugin/chain_api_plugin.hpp>
//#include <eosio/http_plugin/http_plugin.hpp>
#include <../../../libraries/singularity/include/singularity.hpp>
#include <../../../libraries/singularity/generated/git_version.hpp>
//
//
#include <fc/io/json.hpp>
//#include <fc/crypto/sha256.hpp>
//#include <eosio/uos_rates/cvs.h>
//#include <algorithm>
//#include <boost/program_options.hpp>
//#include "../../../../../../libraries/fc/include/fc/variant.hpp"


namespace uos {
    using namespace std;
    using namespace eosio;

    class data_processor {

    public:

        //settings
        int32_t period = 5*60*2;//5 minutes
        uint32_t transaction_window = 100*86400*2;//100 days
        uint32_t activity_window = 30*86400*2; //30 days

        double social_importance_share = 0.1;
        double transfer_importance_share = 0.1;
        double stake_importance_share = 1.0 - social_importance_share - transfer_importance_share;

        const double activity_monetary_value = 1000;
        const uint8_t blocks_per_second = 2;
        const double yearly_emission_percent = 1.0;
        const int64_t initial_token_supply = 1000000000;

        //input
        uint32_t current_calc_block;
        fc::variants source_transactions;
        vector<map<string,string>> balance_snapshot;
        map<string,string> prev_cumulative_emission;
        string prev_max_network_activity = "0";

        //intermediate
        vector<std::shared_ptr<singularity::relation_t>> transfer_relations;
        vector<std::shared_ptr<singularity::relation_t>> social_relations;
        vector<singularity::transaction_t> activity_relations;

        //output
        map<string, fc::mutable_variant_object> accounts;
        map<string, fc::mutable_variant_object> content;

        //calculation details
        singularity::activity_index_detalization_t activity_details;
        singularity::activity_index_detalization_t content_details;

        string network_activity;
        string max_network_activity;
        string full_prev_emission;
        string target_emission;
        string emission_limit;
        string resulting_emission;
        string real_resulting_emission;

        string result_hash;

        explicit data_processor(uint32_t calc_block){
            current_calc_block = calc_block;
        }

        void convert_transactions_to_relations();
        vector<std::shared_ptr<singularity::relation_t>> parse_token_transaction(fc::variant trx);
        vector<std::shared_ptr<singularity::relation_t>> parse_social_transaction(fc::variant trx);
        void add_lost_items(fc::variant trx);

        void calculate_social_rates();
        void calculate_transfer_rates();
        void calculate_stake_rates();
        void calculate_importance();

        void calculate_scaled_values();

        void calculate_network_activity();
        void calculate_emission();

        void calculate_hash();

        static string to_string_4(double value);
        static string to_string_4(singularity::double_type value);
        static string to_string_10(double value);
        static string to_string_10(singularity::double_type value);

        string get_acc_string_value(string acc_name, string value_name);
        double get_acc_double_value(string acc_name, string value_name);
        long get_acc_long_value(string acc_name, string value_name);
        string get_cont_string_value(string cont_name, string value_name);
        double get_cont_double_value(string cont_name, string value_name);
    };

    void data_processor::convert_transactions_to_relations() {

        int32_t end_block = current_calc_block;
        int32_t start_block = end_block - transaction_window + 1;
        if (start_block < 1)
            start_block = 1;

        int32_t activity_end_block = end_block;
        int32_t activity_start_block = end_block - activity_window + 1;
        if (activity_start_block < 1)
            activity_start_block = 1;

        for(auto trx : source_transactions){

            //add the lost items regardless of the block number
            if(trx["acc"].as_string() == "uos.activity") {
                add_lost_items(trx);
            }

            auto block_num = stoi(trx["block_num"].as_string());

            if(block_num < start_block || block_num > end_block)
                continue;

            vector<std::shared_ptr<singularity::relation_t>> relations;

            if(trx["acc"].as_string() == "eosio.token") {
                relations = parse_token_transaction(trx);
                transfer_relations.insert(transfer_relations.end(),relations.begin(), relations.end());
            }

            if(trx["acc"].as_string() == "uos.activity") {
                relations = parse_social_transaction(trx);
                social_relations.insert(social_relations.end(),relations.begin(), relations.end());
            }

            if(block_num < activity_start_block || block_num > activity_end_block)
                continue;

            for(auto rel : relations){
                singularity::transaction_t tran(
                        rel->get_weight(),
                        0,
                        rel->get_source(),
                        rel->get_target(),
                        time_t(0),
                        0,
                        0,
                        rel->get_height());
                activity_relations.emplace_back(tran);
            }
        }
    }

    vector<std::shared_ptr<singularity::relation_t>> data_processor::parse_token_transaction(fc::variant trx){
        auto from = trx["data"]["from"].as_string();
        auto to = trx["data"]["to"].as_string();
        auto quantity = asset::from_string(trx["data"]["quantity"].as_string()).get_amount();
        auto memo = trx["data"]["memo"].as_string();
        auto block_num = stoi(trx["block_num"].as_string());

        transaction_t transfer(quantity,from, to,0 , current_calc_block - block_num);

        vector<std::shared_ptr<singularity::relation_t>> result;
        result.push_back(std::make_shared<transaction_t>(transfer));
        return result;
    }

    vector<std::shared_ptr<singularity::relation_t>> data_processor::parse_social_transaction(fc::variant trx){

        vector<std::shared_ptr<singularity::relation_t>> result;

        //do not even parse if transaction contains \n
        auto json = fc::json::to_string(trx);
        if(json.find("\n") != std::string::npos){
            ilog(json);
            return result;
        }

        auto block_num = stoi(trx["block_num"].as_string());
        auto block_height = current_calc_block - block_num;

        if (trx["action"].as_string() == "makecontent" ) {

            auto from = trx["data"]["acc"].as_string();
            auto to = trx["data"]["content_id"].as_string();
            auto content_type_id = trx["data"]["content_type_id"].as_string();

            //do not use "create orgainzation as the content" events, code 4
            if(content_type_id == "4")
                return result;

            ownership_t ownership(from, to, block_height);
            result.push_back(std::make_shared<ownership_t>(ownership));
        }

        if (trx["action"].as_string() == "usertocont") {

            auto from = trx["data"]["acc"].as_string();
            auto to = trx["data"]["content_id"].as_string();
            auto interaction_type_id = trx["data"]["interaction_type_id"].as_string();

            if(interaction_type_id == "2") {
                upvote_t upvote(from, to, block_height);
                result.push_back(std::make_shared<upvote_t>(upvote));
            }
            if(interaction_type_id == "4") {
                downvote_t downvote(from, to, block_height);
                result.push_back(std::make_shared<downvote_t>(downvote));
            }
        }
        if (trx["action"].as_string() == "makecontorg") {

            auto from = trx["data"]["organization_id"].as_string();
            auto to = trx["data"]["content_id"].as_string();
            ownership_t ownershiporg(from, to, block_height);
            result.push_back(std::make_shared<ownership_t>(ownershiporg));

        }

        return result;
    }

    void data_processor::add_lost_items(fc::variant trx) {
        if (trx["action"].as_string() == "makecontent"){
            auto from = trx["data"]["acc"].as_string();
            auto to = trx["data"]["content_id"].as_string();
            auto content_type_id = trx["data"]["content_type_id"].as_string();

            //add organizations as the accounts
            if(content_type_id == "4"){
                if(accounts.find(to) == accounts.end())
                    accounts[to] = fc::mutable_variant_object();
            }
            //add the content
            else {
                if(content.find(to) == content.end())
                    content[to] = fc::mutable_variant_object();
            }
        }
    }

    void data_processor::calculate_social_rates() {
        singularity::parameters_t params;
        params.include_detailed_data = true;
        params.use_diagonal_elements = true;

        auto social_calculator =
                singularity::rank_calculator_factory::create_calculator_for_social_network(params);

        social_calculator->add_block(social_relations);
        auto social_rates = social_calculator->calculate();

        for(auto item : *social_rates[singularity::ACCOUNT]){
            if(accounts.find(item.first) == accounts.end())
                accounts[item.first] = fc::mutable_variant_object();
            accounts[item.first].set("social_rate", to_string_10(item.second));
        }

        for(auto item : *social_rates[singularity::CONTENT]){
            if(content.find(item.first) == content.end())
                content[item.first] = fc::mutable_variant_object();
            content[item.first].set("social_rate", to_string_10(item.second));
        }

        activity_details = social_calculator->get_detalization();
        content_details = social_calculator->get_content_detalization();
    }

    void data_processor::calculate_transfer_rates() {
        singularity::parameters_t params;

        auto transfer_calculator =
                singularity::rank_calculator_factory::create_calculator_for_transfer(params);

        transfer_calculator->add_block(transfer_relations);
        auto transfer_rates = transfer_calculator->calculate();

        for(auto item : *transfer_rates[singularity::ACCOUNT]){
            if(accounts.find(item.first) == accounts.end())
                accounts[item.first] = fc::mutable_variant_object();
            accounts[item.first].set("transfer_rate", to_string_10(item.second));
        }
    }

    void data_processor::calculate_stake_rates() {

        long total_stake = 0;
        for(auto item : balance_snapshot){
            if(accounts.find(item["name"]) == accounts.end())
                accounts[item["name"]] = fc::mutable_variant_object();

            string cpu_weight = item["cpu_weight"];
            string net_weight = item["net_weight"];

            if(cpu_weight == "-1") cpu_weight = "0";
            if(net_weight == "-1") net_weight = "0";
            long staked_balance = stol(cpu_weight) + stol(net_weight);
            total_stake += staked_balance;

            accounts[item["name"]].set("staked_balance", std::to_string(staked_balance));
        }

        for(auto acc : accounts){
            double stake_rate = get_acc_double_value(acc.first,"staked_balance") / (double) total_stake;
            accounts[acc.first].set("stake_rate", to_string_10(stake_rate));
        }
    }

    void data_processor::calculate_importance() {
        for (auto item : accounts){
            double importance = get_acc_double_value(item.first, "social_rate") * social_importance_share +
                                get_acc_double_value(item.first, "transfer_rate") * transfer_importance_share +
                                get_acc_double_value(item.first, "stake_rate") * stake_importance_share;
            accounts[item.first].set("importance", to_string_10(importance));
        }
    }

    void data_processor::calculate_scaled_values() {
        //auto acc_count = accounts.size();
        ///for acc_count use only accounts with non-zero social_rate
        int acc_count = 0;
        for(auto acc : accounts){
            if(get_acc_string_value(acc.first, "social_rate") != "0")
                acc_count++;
        }
        for(auto acc : accounts){
            double scaled_social_rate = get_acc_double_value(acc.first, "social_rate") * acc_count;
            double scaled_transfer_rate = get_acc_double_value(acc.first, "transfer_rate") * acc_count;
            double scaled_stake_rate = get_acc_double_value(acc.first, "stake_rate") * acc_count;
            double scaled_importance = get_acc_double_value(acc.first, "importance") * acc_count;

            accounts[acc.first].set("scaled_social_rate", to_string_10(scaled_social_rate));
            accounts[acc.first].set("scaled_transfer_rate", to_string_10(scaled_transfer_rate));
            accounts[acc.first].set("scaled_stake_rate", to_string_10(scaled_stake_rate));
            accounts[acc.first].set("scaled_importance", to_string_10(scaled_importance));
        }

        //auto cont_count = content.size();
        ///use the same scale for the content rate
        for(auto cont : content){
            double scaled_social_rate = get_cont_double_value(cont.first, "social_rate") * acc_count;
            content[cont.first].set("scaled_social_rate", to_string_10(scaled_social_rate));
        }
    }

    void data_processor::calculate_network_activity() {
        singularity::activity_period act_period;
        act_period.add_block(activity_relations);
        auto activity = act_period.get_activity();
        network_activity = to_string_10(activity);

        double max_activity_d = stod(prev_max_network_activity);
        if(stod(to_string_10(activity)) > max_activity_d)
            max_activity_d = stod(to_string_10(activity));
        max_network_activity = to_string_10(max_activity_d);
    }

    void data_processor::calculate_emission() {
        singularity::emission_calculator_new em_calculator;
        auto target_emission_d = em_calculator.get_target_emission(stod(network_activity), 0, activity_monetary_value);
        target_emission = to_string_4(target_emission_d);
        auto emission_limit_d = em_calculator.get_emission_limit(initial_token_supply,
                                                               yearly_emission_percent,
                                                               period / blocks_per_second);
        emission_limit = to_string_4(emission_limit_d);

        double full_prev_emission_d = 0;
        for(auto item : prev_cumulative_emission){
            if(accounts.find(item.first) == accounts.end())
                accounts[item.first] = fc::mutable_variant_object();
            accounts[item.first].set("prev_cumulative_emission", item.second);
            full_prev_emission_d += stod(item.second);
        }
        full_prev_emission = to_string_4(full_prev_emission_d);

        auto resulting_emission_d = em_calculator.get_resulting_emission(
                stod(target_emission) - full_prev_emission_d, stod(emission_limit), 0.5);
        resulting_emission = to_string_4(resulting_emission_d);


        double real_resulting_emission_d = 0;
        for(auto acc : accounts){
            double current_emission_d = stod(resulting_emission) * get_acc_double_value(acc.first, "importance");
            accounts[acc.first].set("current_emission", to_string_4(current_emission_d));
            double cumulative_emission = get_acc_double_value(acc.first, "prev_cumulative_emission") +
                                         get_acc_double_value(acc.first, "current_emission");
            accounts[acc.first].set("current_cumulative_emission", to_string_4(cumulative_emission));

            real_resulting_emission_d += get_acc_double_value(acc.first, "current_emission");
        }
        real_resulting_emission = to_string_4(real_resulting_emission_d);
    }

    void data_processor::calculate_hash() {
        uos::merkle_tree<string> mtree;
        vector< pair< string, string> > mt_input;
        for(auto acc : accounts){

            string str_statement = "emission " + acc.first +
                                   " " + get_acc_string_value(acc.first, "current_cumulative_emission");
            mt_input.emplace_back(make_pair(str_statement, str_statement));
        }
        mtree.set_accounts(mt_input);
        mtree.count_tree();
        result_hash = string(mtree.nodes_list[mtree.nodes_list.size() - 1][0]);
    }

    string data_processor::to_string_4(double value) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(4) << value;
        return ss.str();
    }

    string data_processor::to_string_4(singularity::double_type value) {
        return value.str(4,ios_base::fixed);
    }

    string data_processor::to_string_10(double value) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(10) << value;
        return ss.str();
    }

    string data_processor::to_string_10(singularity::double_type value) {
        return value.str(10,ios_base::fixed);
    }

    string data_processor::get_acc_string_value(std::string acc_name, std::string value_name) {
        if(accounts.find(acc_name) == accounts.end())
            return "0";

        if(accounts[acc_name].find(value_name) == accounts[acc_name].end())
            return "0";

        auto str_value = accounts[acc_name][value_name].as_string();
        return str_value;
    }

    double data_processor::get_acc_double_value(std::string acc_name, std::string value_name) {
        auto str_value = get_acc_string_value(acc_name, value_name);
        return stod(str_value);
    }

    long data_processor::get_acc_long_value(std::string acc_name, std::string value_name) {
        auto str_value = get_acc_string_value(acc_name, value_name);
        return stol(str_value);
    }

    string data_processor::get_cont_string_value(std::string cont_name, std::string value_name) {
        if(content.find(cont_name) == content.end())
            return "0";

        if(content[cont_name].find(value_name) == content[cont_name].end())
            return "0";

        auto str_value = content[cont_name][value_name].as_string();
        return str_value;
    }

    double data_processor::get_cont_double_value(std::string cont_name, std::string value_name) {
        auto str_value = get_cont_string_value(cont_name, value_name);
        return stod(str_value);
    }
}
