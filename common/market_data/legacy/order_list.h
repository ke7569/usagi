#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <list>
class OrderList {
public:
    OrderList();

    void insert_quote_tag(int quote_volume, bool is_cancel, long quote_tag);
    void cross_trade_volume(int total_trade_volume);


    inline int get_volume() const{
        return mVolume;
    }
    inline void set_volume(int volume) {
        mVolume=volume;
    }


private:
    int mVolume;
};



