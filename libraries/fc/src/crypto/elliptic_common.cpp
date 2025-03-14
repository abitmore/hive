#include <fc/crypto/base58.hpp>
#include <fc/crypto/elliptic.hpp>
#include <fc/io/raw.hpp>
#include <fc/crypto/hmac.hpp>
#include <fc/crypto/openssl.hpp>
#include <fc/crypto/ripemd160.hpp>

#include <boost/endian/conversion.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#ifdef _WIN32
# include <malloc.h>
#else
# include <alloca.h>
#endif

/* stuff common to all ecc implementations */

#define BTC_EXT_PUB_MAGIC   (0x0488B21E)
#define BTC_EXT_PRIV_MAGIC  (0x0488ADE4)

namespace fc { namespace ecc {

   using namespace boost::multiprecision::literals;

    namespace detail {
        typedef fc::array<char,37> chr37;

        fc::sha256 _left( const fc::sha512& v )
        {
            fc::sha256 result;
            memcpy( result.data(), v.data(), 32 );
            return result;
        }

        fc::sha256 _right( const fc::sha512& v )
        {
            fc::sha256 result;
            memcpy( result.data(), v.data() + 32, 32 );
            return result;
        }

        static void _put( unsigned char** dest, unsigned int i)
        {
            *(*dest)++ = (i >> 24) & 0xff;
            *(*dest)++ = (i >> 16) & 0xff;
            *(*dest)++ = (i >>  8) & 0xff;
            *(*dest)++ =  i        & 0xff;
        }

        static unsigned int _get( unsigned char** src )
        {
            unsigned int result = *(*src)++ << 24;
            result |= *(*src)++ << 16;
            result |= *(*src)++ <<  8;
            result |= *(*src)++;
            return result;
        }

        static chr37 _derive_message( char first, const char* key32, int i )
        {
            chr37 result;
            unsigned char* dest = (unsigned char*) result.begin();
            *dest++ = first;
            memcpy( dest, key32, 32 ); dest += 32;
            _put( &dest, i );
            return result;
        }

        chr37 _derive_message( const public_key_data& key, int i )
        {
            return _derive_message( *key.begin(), key.begin() + 1, i );
        }

        static chr37 _derive_message( const private_key_secret& key, int i )
        {
            return _derive_message( 0, key.data(), i );
        }

        const ec_group& get_curve()
        {
            static const ec_group secp256k1( EC_GROUP_new_by_curve_name( NID_secp256k1 ) );
            return secp256k1;
        }

        static private_key_secret _get_curve_order()
        {
            const ec_group& group = get_curve();
            bn_ctx ctx(BN_CTX_new());
            ssl_bignum order;
            FC_ASSERT( EC_GROUP_get_order( group, order, ctx ) );
            private_key_secret bin;
            FC_ASSERT( static_cast<size_t>(BN_num_bytes( order )) == bin.data_size() );
            FC_ASSERT( static_cast<size_t>(BN_bn2bin( order, (unsigned char*) bin.data() )) == bin.data_size() );
            return bin;
        }

        const private_key_secret& get_curve_order()
        {
            static private_key_secret order = _get_curve_order();
            return order;
        }

        static private_key_secret _get_half_curve_order()
        {
            const ec_group& group = get_curve();
            bn_ctx ctx(BN_CTX_new());
            ssl_bignum order;
            FC_ASSERT( EC_GROUP_get_order( group, order, ctx ) );
            BN_rshift1( order, order );
            private_key_secret bin;
            FC_ASSERT( static_cast<size_t>(BN_num_bytes( order )) == bin.data_size() );
            FC_ASSERT( static_cast<size_t>(BN_bn2bin( order, (unsigned char*) bin.data() )) == bin.data_size() );
            return bin;
        }

        const private_key_secret& get_half_curve_order()
        {
            static private_key_secret half_order = _get_half_curve_order();
            return half_order;
        }
    }

    public_key public_key::from_key_data( const public_key_data &data ) {
        return public_key(data);
    }

    public_key public_key::child( const fc::sha256& offset )const
    {
       fc::sha256::encoder enc;
       fc::raw::pack( enc, *this );
       fc::raw::pack( enc, offset );

       return add( enc.result() );
    }

    private_key private_key::child( const fc::sha256& offset )const
    {
       fc::sha256::encoder enc;
       fc::raw::pack( enc, get_public_key() );
       fc::raw::pack( enc, offset );
       return generate_from_seed( get_secret(), enc.result() );
    }

    std::string public_key::to_base58( const public_key_data &key, bool is_sha256 )
    {
      uint32_t check = 0;

      if( is_sha256 )
        check = (uint32_t)sha256::hash(key.data, sizeof(key))._hash[0];
      else
        check = (uint32_t)ripemd160::hash(key.data, sizeof(key))._hash[0];

      assert(key.size() + sizeof(check) == 37);
      array<char, 37> data;
      memcpy(data.data, key.begin(), key.size());
      memcpy(data.begin() + key.size(), (const char*)&check, sizeof(check));
      return fc::to_base58(data.begin(), data.size());
    }

    std::string public_key::to_base58_with_prefix(const std::string& prefix ) const
    {
      return to_base58_with_prefix( serialize(), prefix );
    }

    std::string public_key::to_base58_with_prefix( const public_key_data &key, const std::string& prefix, bool is_sha256 )
    {
      return prefix + to_base58( key, is_sha256 );
    }

    public_key public_key::from_base58( const std::string& b58, bool is_sha256 )
    {
        array<char, 37> data;
        size_t s = fc::from_base58(b58, (char*)&data, sizeof(data) );
        FC_ASSERT( s == sizeof(data) );

        public_key_data key;
        uint32_t check = 0;

        if( is_sha256 )
            check = (uint32_t)sha256::hash(data.data, sizeof(key))._hash[0];
        else
            check = (uint32_t)ripemd160::hash(data.data, sizeof(key))._hash[0];

        FC_ASSERT( memcmp( (char*)&check, data.data + sizeof(key), sizeof(check) ) == 0 );
        memcpy( (char*)key.data, data.data, sizeof(key) );
        return from_key_data(key);
    }

    public_key public_key::from_base58_with_prefix( const std::string& b58_with_prefix, const std::string& prefix )
    {
      // TODO:  Refactor syntactic checks into static is_valid()
      //        to make public_key_type API more similar to address API
      const size_t prefix_len = prefix.size();
      FC_ASSERT( b58_with_prefix.size() > prefix_len );
      FC_ASSERT( b58_with_prefix.substr( 0, prefix_len ) ==  prefix , "", ("b58_with_prefix", b58_with_prefix) );

      return from_base58( b58_with_prefix.substr( prefix_len ), false/*is_sha256*/ );
    }

    unsigned int public_key::fingerprint() const
    {
        public_key_data key = serialize();
        ripemd160 hash = ripemd160::hash( sha256::hash( key.begin(), key.size() ) );
        unsigned char* fp = (unsigned char*) hash._hash;
        return (fp[0] << 24) | (fp[1] << 16) | (fp[2] << 8) | fp[3];
    }

    bool is_bip_0062_canonical( const compact_signature& c )
    {
       using boost::multiprecision::uint256_t;
       constexpr uint256_t n_2 =
          0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF5D576E7357A4501DDFE92F46681B20A0_cppui256;

       // BIP-0062 states that sig must be in [1,n/2], however because a sig of value 0 is an invalid
       // signature under all circumstances, the lower bound does not need checking
       return memcmp( c.data + 33, &n_2, sizeof( uint256_t ) ) <= 0;
    }


    bool public_key::is_canonical( const compact_signature& c )
    {
      return is_bip_0062_canonical( c );
    }

    private_key private_key::generate_from_seed( const fc::sha256& seed, const fc::sha256& offset )
    {
        ssl_bignum z;
        BN_bin2bn((unsigned char*)&offset, sizeof(offset), z);

        ec_group group(EC_GROUP_new_by_curve_name(NID_secp256k1));
        bn_ctx ctx(BN_CTX_new());
        ssl_bignum order;
        EC_GROUP_get_order(group, order, ctx);

        // secexp = (seed + z) % order
        ssl_bignum secexp;
        BN_bin2bn((unsigned char*)&seed, sizeof(seed), secexp);
        BN_add(secexp, secexp, z);
        BN_mod(secexp, secexp, order, ctx);

        fc::sha256 secret;
        assert(BN_num_bytes(secexp) <= int64_t(sizeof(secret)));
        auto shift = sizeof(secret) - BN_num_bytes(secexp);
        BN_bn2bin(secexp, ((unsigned char*)&secret)+shift);
        return regenerate( secret );
    }

    fc::optional<private_key> private_key::generate_from_base58( const std::string& b58 )
    {
      std::vector<char> b58_bytes;
      try
      {
        b58_bytes = fc::from_base58(b58);
      }
      catch (const fc::parse_error_exception&)
      {
        return fc::optional<fc::ecc::private_key>();
      }
      if (b58_bytes.size() < 5)
        return fc::optional<fc::ecc::private_key>();
      std::vector<char> key_bytes(b58_bytes.begin() + 1, b58_bytes.end() - 4);
      fc::ecc::private_key key = fc::variant(key_bytes).as<fc::ecc::private_key>();
      fc::sha256 check = fc::sha256::hash(b58_bytes.data(), b58_bytes.size() - 4);
      fc::sha256 check2 = fc::sha256::hash(check);

      if( memcmp( (char*)&check, b58_bytes.data() + b58_bytes.size() - 4, 4 ) == 0 ||
          memcmp( (char*)&check2, b58_bytes.data() + b58_bytes.size() - 4, 4 ) == 0 )
        return key;

      return fc::optional<fc::ecc::private_key>();
    }

    fc::optional<private_key> private_key::wif_to_key( const std::string& wif )
    {
      return generate_from_base58( wif );
    }

    fc::sha256 private_key::get_secret( const EC_KEY * const k )
    {
       if( !k )
       {
          return fc::sha256();
       }

       fc::sha256 sec;
       const BIGNUM* bn = EC_KEY_get0_private_key(k);
       if( bn == NULL )
       {
         FC_THROW_EXCEPTION( exception, "get private key failed" );
       }
       int nbytes = BN_num_bytes(bn);
       BN_bn2bin(bn, &((unsigned char*)&sec)[32-nbytes] );
       return sec;
    }

    std::string private_key::to_base58() const
    {
      auto secret = get_secret();

      const size_t size_of_data_to_hash = sizeof(secret) + 1;
      const size_t size_of_hash_bytes = 4;
      char data[size_of_data_to_hash + size_of_hash_bytes];

      data[0] = (char)0x80;
      memcpy(&data[1], (char*)&secret, sizeof(secret));
      fc::sha256 digest = fc::sha256::hash(data, size_of_data_to_hash);
      digest = fc::sha256::hash(digest);
      memcpy(data + size_of_data_to_hash, (char*)&digest, size_of_hash_bytes);

      return fc::to_base58(data, sizeof(data));
    }

    std::string private_key::key_to_wif() const
    {
      return to_base58();
    }

    private_key private_key::generate()
    {
       EC_KEY* k = EC_KEY_new_by_curve_name( NID_secp256k1 );
       if( !k ) FC_THROW_EXCEPTION( exception, "Unable to generate EC key" );
       if( !EC_KEY_generate_key( k ) )
       {
          FC_THROW_EXCEPTION( exception, "ecc key generation error" );

       }

       return private_key( k );
    }

    static fc::string _to_base58( const extended_key_data& key )
    {
        size_t buffer_size = key.size() + 4;
        char *buffer = (char*)alloca(buffer_size);
        memcpy( buffer, key.begin(), key.size() );
        fc::sha256 double_hash = fc::sha256::hash( fc::sha256::hash( key.begin(), key.size() ));
        memcpy( buffer + key.size(), double_hash.data(), 4 );
        return fc::to_base58( buffer, buffer_size );
    }

    static void _parse_extended_data( unsigned char* buffer, fc::string base58 )
    {
        memset( buffer, 0, 78 );
        std::vector<char> decoded = fc::from_base58( base58 );
        unsigned int i = 0;
        for ( char c : decoded )
        {
            if ( i >= 78 || i > decoded.size() - 4 ) { break; }
            buffer[i++] = c;
        }
    }

    extended_public_key extended_public_key::derive_child(int i) const
    {
        FC_ASSERT( !(i&0x80000000), "Can't derive hardened public key!" );
        return derive_normal_child(i);
    }

    extended_key_data extended_public_key::serialize_extended() const
    {
        extended_key_data result;
        unsigned char* dest = (unsigned char*) result.begin();
        detail::_put( &dest, BTC_EXT_PUB_MAGIC );
        *dest++ = depth;
        detail::_put( &dest, parent_fp );
        detail::_put( &dest, child_num );
        memcpy( dest, c.data(), c.data_size() ); dest += 32;
        public_key_data key = serialize();
        memcpy( dest, key.begin(), key.size() );
        return result;
    }

    extended_public_key extended_public_key::deserialize( const extended_key_data& data )
    {
       return from_base58( _to_base58( data ) );
    }

    fc::string extended_public_key::str() const
    {
        return _to_base58( serialize_extended() );
    }

    extended_public_key extended_public_key::from_base58( const fc::string& base58 )
    {
        unsigned char buffer[78];
        unsigned char* ptr = buffer;
        _parse_extended_data( buffer, base58 );
        FC_ASSERT( detail::_get( &ptr ) == BTC_EXT_PUB_MAGIC, "Invalid extended private key" );
        uint8_t d = *ptr++;
        int fp = detail::_get( &ptr );
        int cn = detail::_get( &ptr );
        fc::sha256 chain;
        memcpy( chain.data(), ptr, chain.data_size() ); ptr += chain.data_size();
        public_key_data key;
        memcpy( key.begin(), ptr, key.size() );
        return extended_public_key( key, chain, cn, fp, d );
    }

    extended_public_key extended_private_key::get_extended_public_key() const
    {
        return extended_public_key( get_public_key(), c, child_num, parent_fp, depth );
    }

    public_key extended_public_key::generate_p(int i) const { return derive_normal_child(2*i + 0); }
    public_key extended_public_key::generate_q(int i) const { return derive_normal_child(2*i + 1); }

    extended_private_key extended_private_key::derive_child(int i) const
    {
        return i < 0 ? derive_hardened_child(i) : derive_normal_child(i);
    }

    extended_private_key extended_private_key::derive_normal_child(int i) const
    {
        const detail::chr37 data = detail::_derive_message( get_public_key().serialize(), i );
        hmac_sha512 mac;
        fc::sha512 l = mac.digest( c.data(), c.data_size(), data.begin(), data.size() );
        return private_derive_rest( l, i );
    }

    extended_private_key extended_private_key::derive_hardened_child(int i) const
    {
        hmac_sha512 mac;
        private_key_secret key = get_secret();
        const detail::chr37 data = detail::_derive_message( key, i );
        fc::sha512 l = mac.digest( c.data(), c.data_size(), data.begin(), data.size() );
        return private_derive_rest( l, i );
    }

    extended_key_data extended_private_key::serialize_extended() const
    {
        extended_key_data result;
        unsigned char* dest = (unsigned char*) result.begin();
        detail::_put( &dest, BTC_EXT_PRIV_MAGIC );
        *dest++ = depth;
        detail::_put( &dest, parent_fp );
        detail::_put( &dest, child_num );
        memcpy( dest, c.data(), c.data_size() ); dest += 32;
        *dest++ = 0;
        private_key_secret key = get_secret();
        memcpy( dest, key.data(), key.data_size() );
        return result;
    }

    extended_private_key extended_private_key::deserialize( const extended_key_data& data )
    {
       return from_base58( _to_base58( data ) );
    }

    private_key extended_private_key::generate_a(int i) const { return derive_hardened_child(4*i + 0); }
    private_key extended_private_key::generate_b(int i) const { return derive_hardened_child(4*i + 1); }
    private_key extended_private_key::generate_c(int i) const { return derive_hardened_child(4*i + 2); }
    private_key extended_private_key::generate_d(int i) const { return derive_hardened_child(4*i + 3); }

    fc::string extended_private_key::str() const
    {
        return _to_base58( serialize_extended() );
    }

    extended_private_key extended_private_key::from_base58( const fc::string& base58 )
    {
        unsigned char buffer[78];
        unsigned char* ptr = buffer;
        _parse_extended_data( buffer, base58 );
        FC_ASSERT( detail::_get( &ptr ) == BTC_EXT_PRIV_MAGIC, "Invalid extended private key" );
        uint8_t d = *ptr++;
        int fp = detail::_get( &ptr );
        int cn = detail::_get( &ptr );
        fc::sha256 chain;
        memcpy( chain.data(), ptr, chain.data_size() ); ptr += chain.data_size();
        ptr++;
        private_key_secret key;
        memcpy( key.data(), ptr, key.data_size() );
        return extended_private_key( private_key::regenerate(key), chain, cn, fp, d );
    }

    extended_private_key extended_private_key::generate_master( const fc::string& seed )
    {
        return generate_master( seed.c_str(), seed.size() );
    }

    extended_private_key extended_private_key::generate_master( const char* seed, uint32_t seed_len )
    {
        hmac_sha512 mac;
        fc::sha512 hash = mac.digest( "Bitcoin seed", 12, seed, seed_len );
        extended_private_key result( private_key::regenerate( detail::_left(hash) ),
                                     detail::_right(hash) );
        return result;
    }
}

void to_variant( const ecc::private_key& var,  variant& vo )
{
    vo = var.get_secret();
}

void from_variant( const variant& var,  ecc::private_key& vo )
{
    fc::sha256 sec;
    from_variant( var, sec );
    vo = ecc::private_key::regenerate(sec);
}

void to_variant( const ecc::public_key& var,  variant& vo )
{
    vo = var.serialize();
}

void from_variant( const variant& var,  ecc::public_key& vo )
{
    ecc::public_key_data dat;
    from_variant( var, dat );
    vo = ecc::public_key(dat);
}

}
